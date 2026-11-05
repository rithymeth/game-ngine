plugins {
    id("com.android.application")
}

val releaseStoreFile = providers.environmentVariable("AETHER_ANDROID_KEYSTORE").orNull
val releaseStorePassword = providers.environmentVariable("AETHER_ANDROID_STORE_PASSWORD").orNull
val releaseKeyAlias = providers.environmentVariable("AETHER_ANDROID_KEY_ALIAS").orNull
val releaseKeyPassword = providers.environmentVariable("AETHER_ANDROID_KEY_PASSWORD").orNull
val hasReleaseSigning = listOf(releaseStoreFile, releaseStorePassword, releaseKeyAlias, releaseKeyPassword)
    .all { !it.isNullOrBlank() }

android {
    namespace = "com.aether.engine"
    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "com.aether.engine"
        minSdk = 28
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // arm64-v8a ships; CI adds x86_64 (-Paether.abis=x86_64) to run the app on an x86_64 emulator.
            abiFilters += (providers.gradleProperty("aether.abis").orNull ?: "arm64-v8a").split(",")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DAETHER_BUILD_ANDROID_APP=ON",
                    "-DAETHER_BUILD_TESTS=OFF",
                    "-DAETHER_BUILD_MCP=OFF",
                    "-DAETHER_BUILD_SANDBOX=OFF",
                    "-DAETHER_BUILD_PHYSICS=OFF",
                    "-DAETHER_BUILD_EDITOR=OFF",
                    "-DAETHER_BUILD_EDITOR_UI=OFF",
                    "-DAETHER_BUILD_VULKAN=ON",
                    "-DAETHER_BUILD_SCRIPTING=OFF",
                    "-DAETHER_BUILD_WINDOWING=OFF",
                    "-DAETHER_BUILD_BENCH=OFF",
                )
                targets += "aether_android"
            }
        }
    }

    // CI cooks a game and passes its folder (holding Paks/*.apak) with -Paether.assets=<dir>; the app copies the paks
    // out in scene mode. Without it the APK has no game and scene mode reports that.
    providers.gradleProperty("aether.assets").orNull?.let { dir ->
        sourceSets.getByName("main").assets.srcDir(dir)
    }

    externalNativeBuild {
        cmake {
            path = file("../../../CMakeLists.txt")
            version = "3.22.1"
        }
    }

    if (hasReleaseSigning) {
        signingConfigs {
            create("release") {
                storeFile = file(releaseStoreFile!!)
                storePassword = releaseStorePassword
                keyAlias = releaseKeyAlias
                keyPassword = releaseKeyPassword
            }
        }
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            if (hasReleaseSigning) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }
}
