// Top-level build file. Individual module config lives in app/build.gradle.kts
plugins {
    id("com.android.application") version "9.4.1" apply false
}

tasks.register("clean", Delete::class) {
    delete(rootProject.layout.buildDirectory)
}