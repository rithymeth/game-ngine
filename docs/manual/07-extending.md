# Plugins and editor extensions

## Plugins

A plugin is a folder with a `.aplugin` descriptor: a name, a version, the plugins it
depends on, and its modules, each **runtime** or **editor** and loaded in a
phase (PreDefault, Default, PostDefault). Physics, Audio, Navigation, AI and
Networking are plugins. A module is a class registered with `AETHER_MODULE`
with `Startup` and `Shutdown`; dependencies start first, and a cycle is
reported.

### Make your own plugin

```
aether_plugin new Coins --dir MyGame/Plugins
```

creates `MyGame/Plugins/Coins/Coins.aplugin` and an empty `Content/` beside it. Put scenes, scripts, Blueprints and assets in
`Content/`, add `"Coins"` to the project's `plugins` list, and `aether_package` ships them under `Content/Plugins/Coins/`. Run
`aether_plugin package MyGame/Plugins/Coins -o Coins.apkg` to share it with another project.

A plugin made this way is **content only**: it declares no module, because there is no code to start. `--module` also declares a
runtime module of the same name, for a plugin whose C++ registers `AETHER_MODULE(Coins, ...)`; that class has to be built into
the player, or the player warns that the module isn't built in every time it starts. The editor's Plugins window makes the same
content-only scaffold.

## Extending the editor

The editor's **extension registry** takes four kinds of addition, each owned by
a plugin or script so they leave together:

- **Panels**: a window listed in **Tools** under *Extensions*.
- **Menu items**: `"Menu/Sub/Item"` paths with an optional shortcut. Paths under
  `Tools/` join the Tools menu; any other root gets a menu of its own.
- **Property drawers**: replace the Inspector's widget for every field of a
  reflected type.
- **Asset types**: an extension, a template for new files, and an opener.

From C++, an editor module registers into `ExtensionRegistry::Active()`. From
Luau, drop a script in `Content/Editor/`:

    local clicks = 0
    editor.AddPanel("Hello Panel", function()
        if ui.Button("Click me") then clicks += 1 end
        ui.SameLine()
        ui.Text("clicks: " .. clicks)
    end)
    editor.AddMenuItem("Tools/Hello/Say hello", function() editor.Log("hi") end, "Ctrl+Shift+H")
    editor.AddAssetType("Dialogue", ".dialogue", "{}")

The widgets are `ui.Text`, `TextDisabled`, `Button`, `Checkbox`, `SliderFloat`,
`InputText`, `CollapsingHeader`, `SameLine`, `Separator` and `Spacing`; those
that edit a value return the new one (`on = ui.Checkbox("On", on)`). A script
that fails to load leaves nothing behind, and an error in a panel shows in the
panel. The editor's sample project ships a working `Hello.luau`.
