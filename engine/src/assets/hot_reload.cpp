#include "aether/assets/hot_reload.h"

#include <algorithm>
#include <set>
#include <unordered_map>

namespace aether::assets {

namespace {

std::string SettingsText(AssetDatabase& database, const AssetGuid& guid) {
    AssetMeta meta;
    return LoadAssetMeta(database.MetaPath(guid), meta) ? meta.settings.dump() : std::string();
}

std::vector<AssetGuid> DependentsOf(const AssetDatabase& database, const AssetGuid& guid) {
    std::vector<AssetGuid> dependents;
    auto add_users = [&](const AssetGuid& used) {
        for (const AssetRecord* user : database.Referencers(used)) {
            if (std::find(dependents.begin(), dependents.end(), user->guid) == dependents.end()) {
                dependents.push_back(user->guid);
            }
        }
    };
    add_users(guid);
    if (const AssetRecord* record = database.Find(guid)) {
        for (const AssetGuid& sub : record->sub_assets) {
            add_users(sub);
        }
    }
    std::sort(dependents.begin(), dependents.end());
    return dependents;
}

} // namespace

HotReloader::HotReloader(AssetDatabase& database, const ImporterRegistry& importers, DerivedDataCache& cache,
                         std::chrono::milliseconds debounce, std::string platform)
    : database_(database), importers_(importers), cache_(cache), platform_(std::move(platform)),
      watcher_(database.ContentRoot(), debounce) {
    for (const AssetRecord* record : database_.All()) {
        if (!record->IsSubAsset()) {
            settings_[record->guid] = SettingsText(database_, record->guid);
        }
    }
}

std::vector<AssetChange> HotReloader::Update(FileWatcher::Clock::time_point now) {
    const std::vector<FileChange> files = watcher_.Poll(now);
    if (files.empty()) {
        return {};
    }

    struct Before {
        std::string path;
        bool missing;
    };
    std::unordered_map<AssetGuid, Before> before;
    for (const AssetRecord* record : database_.All()) {
        if (!record->IsSubAsset()) {
            before[record->guid] = {record->path, record->missing};
        }
    }
    std::set<std::string> meta_changed; // source paths whose .ameta changed
    const std::string meta_ext = AssetMeta::kExtension;
    for (const FileChange& file : files) {
        if (file.path.size() > meta_ext.size() &&
            file.path.compare(file.path.size() - meta_ext.size(), meta_ext.size(), meta_ext) == 0) {
            meta_changed.insert(file.path.substr(0, file.path.size() - meta_ext.size()));
        }
    }

    database_.Scan();

    // GUIDs first: importing adds and removes sub-asset records.
    std::vector<AssetGuid> sources;
    for (const AssetRecord* record : database_.All()) {
        if (!record->IsSubAsset()) {
            sources.push_back(record->guid);
        }
    }

    std::vector<AssetChange> changes;
    for (const AssetGuid& guid : sources) {
        const AssetRecord* record = database_.Find(guid);
        const std::string path = record->path;
        const bool missing = record->missing;
        auto old = before.find(guid);

        bool import = false;
        AssetChange::Kind kind = AssetChange::Kind::Reimported;
        if (old == before.end()) {
            kind = AssetChange::Kind::Added;
            import = !missing;
        } else {
            if (old->second.path != path) {
                AssetChange moved;
                moved.kind = AssetChange::Kind::Moved;
                moved.guid = guid;
                moved.path = path;
                moved.old_path = old->second.path;
                moved.dependents = DependentsOf(database_, guid);
                changes.push_back(std::move(moved));
            }
            if (missing) {
                if (!old->second.missing) {
                    AssetChange gone;
                    gone.kind = AssetChange::Kind::Missing;
                    gone.guid = guid;
                    gone.path = path;
                    gone.dependents = DependentsOf(database_, guid);
                    changes.push_back(std::move(gone));
                }
            } else if (record->needs_import || old->second.missing) {
                import = true; // edited, or back after being missing
            } else if (meta_changed.count(path) != 0 && SettingsText(database_, guid) != settings_[guid]) {
                import = true; // import settings edited by hand
            }
        }
        settings_[guid] = SettingsText(database_, guid);
        const std::string attempt = record->source_hash + "|" + settings_[guid];
        if (auto failed = failed_.find(guid); failed != failed_.end() && failed->second == attempt) {
            import = false; // this exact input already failed; wait for a change
        }
        if (!import) {
            continue;
        }

        AssetChange change;
        change.kind = kind;
        change.guid = guid;
        change.path = path;
        if (importers_.Find(record->importer) != nullptr) {
            change.output = ImportAsset(database_, guid, importers_, cache_, platform_);
            if (change.output.ok) {
                failed_.erase(guid);
            } else {
                change.kind = AssetChange::Kind::Failed;
                failed_[guid] = attempt;
            }
            // The import rewrote the .ameta; that's not a change to react to.
            watcher_.Refresh(path + meta_ext);
        } else if (kind == AssetChange::Kind::Reimported) {
            continue; // no importer for this type: nothing to reload
        }
        if (kind == AssetChange::Kind::Added) {
            watcher_.Refresh(path + meta_ext); // Scan created it
        }
        change.dependents = DependentsOf(database_, guid);
        changes.push_back(std::move(change));
    }
    return changes;
}

} // namespace aether::assets
