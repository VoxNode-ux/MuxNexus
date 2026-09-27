# Keep JNI-facing classes/methods so R8 doesn't rename what native code calls by signature.
-keep class com.offlineremux.app.engine.NativeBridge { *; }
-keepclassmembers class com.offlineremux.app.engine.NativeBridge {
    private void onTelemetry(long, long, int, long, int, boolean);
}
