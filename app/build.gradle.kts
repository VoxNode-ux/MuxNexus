plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "com.muxnexus.app"
    compileSdk = 37
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "com.muxnexus.app"
        minSdk = 29
        targetSdk = 37
        versionCode = 1
        versionName = "1.0.0"

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    splits {
        abi {
            isEnable = true
            reset()
            include("arm64-v8a", "armeabi-v7a", "x86_64")
            isUniversalApk = true
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../native/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
        debug {
            isMinifyEnabled = false
            isShrinkResources = false
        }
    }

    bundle {
        language { enableSplit = true }
        density { enableSplit = true }
        abi { enableSplit = true }
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }

    packaging {
        jniLibs.useLegacyPackaging = false
    }

    testOptions {
        unitTests {
            isIncludeAndroidResources = true
            // Robolectric downloads its Android jar at test time. Maven Central often
            // rejects (403/429) GitHub runner IPs, so use Google's Central mirror.
            all { test ->
                test.systemProperty(
                    "robolectric.dependency.repo.url",
                    "https://maven-central.storage-download.googleapis.com/maven2"
                )
            }
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.19.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.11.0")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.11.0")
    implementation("androidx.activity:activity-compose:1.13.0")
    implementation(platform("androidx.compose:compose-bom:2026.09.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material3:material3-window-size-class")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.11.0")
    implementation("com.google.android.material:material:1.14.0")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.16.1")
    testImplementation("androidx.test:core:1.7.0")
    androidTestImplementation("androidx.test.ext:junit:1.3.0")
    androidTestImplementation(platform("androidx.compose:compose-bom:2026.09.00"))
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
}

// Libraries that Android Lint's own classpath (androidLintTool) pulls in
// transitively. They only run on the build machine and are never packaged into
// the APK, but GitHub's dependency graph sees them, so raise them to patched
// releases. The buildscript constraints in the root build.gradle.kts do not
// reach this configuration. Only this one configuration is targeted: Gradle
// rejects constraints on others such as `archives`.
val patchedBuildToolLibs = listOf(
    "org.bouncycastle:bcprov-jdk18on:1.86",
    "org.bouncycastle:bcpkix-jdk18on:1.86",
    "com.google.guava:guava:33.7.2-jre",
    "org.apache.commons:commons-lang3:3.21.0",
    "org.apache.httpcomponents:httpclient:4.5.14",
)
configurations.matching { it.name == "androidLintTool" }.configureEach {
    patchedBuildToolLibs.forEach { notation ->
        dependencyConstraints.add(project.dependencies.constraints.create(notation))
    }
}

// Robolectric puts bcprov 1.81 and guava 33.4.8 on the unit-test classpaths.
// Raise just those two there. Build/test-time only, never packaged into the APK.
configurations.matching { it.name.contains("UnitTest") }.configureEach {
    resolutionStrategy.eachDependency {
        if (requested.group == "org.bouncycastle" && requested.name == "bcprov-jdk18on") {
            useVersion("1.86")
            because("patched Bouncy Castle; Robolectric brings 1.81")
        }
        if (requested.group == "com.google.guava" && requested.name == "guava") {
            useVersion("33.7.2-jre")
            because("patched Guava; Robolectric brings 33.4.8")
        }
    }
}