// Top-level build file. Individual module config lives in app/build.gradle.kts

// These libraries are not part of the app. They come in through the Android Gradle
// Plugin and only run on the build machine. The constraints below force patched
// versions to clear the Dependabot alerts.
buildscript {
    dependencies {
        constraints {
            classpath("org.bouncycastle:bcprov-jdk18on:1.86") { because("CVE-2026-8763, CVE-2026-13506") }
            classpath("org.bouncycastle:bcpkix-jdk18on:1.86") { because("CVE-2026-5588") }
            classpath("org.bouncycastle:bcutil-jdk18on:1.86") { because("keep in step with bcprov/bcpkix") }
            classpath("org.jdom:jdom2:2.0.6.1") { because("XXE in JDOM (CVE-2021-33813)") }
            classpath("com.google.guava:guava:33.7.2-jre") { because("deserialization memory allocation DoS") }
            classpath("org.bitbucket.b_c:jose4j:0.9.6") { because("CVE-2024-29371, compressed JWE DoS") }
            classpath("org.apache.commons:commons-lang3:3.18.0") { because("CVE-2025-48924, uncontrolled recursion") }
            classpath("org.apache.httpcomponents:httpclient:4.5.14") { because("CVE-2020-13956") }
        }
    }
}

plugins {
    id("com.android.application") version "9.5.1" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.4.20" apply false
}

tasks.register("clean", Delete::class) {
    delete(rootProject.layout.buildDirectory)
}
