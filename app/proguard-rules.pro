# Keep JNI-facing classes and the exact method signatures native code resolves by name.
-keep class com.muxnexus.app.engine.NativeBridge { *; }
-keepclassmembers class com.muxnexus.app.engine.NativeBridge {
    private void onTelemetry(long, long, int, long, int, boolean);
}

# The named progress callback interface must survive R8 untouched.
-keep interface com.muxnexus.app.engine.RemuxProgressCallback { *; }
-keep class * implements com.muxnexus.app.engine.RemuxProgressCallback {
    public void onProgress(int, int, java.lang.String);
}
