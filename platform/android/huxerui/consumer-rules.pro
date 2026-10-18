-keep class org.huxerui.HuxerUIApplication { *; }
# These host callbacks are resolved by native code rather than Java call sites.
-keep class org.huxerui.HuxerUIView { *; }
-keep class org.huxerui.HuxerUITextLayout { *; }
-keep class org.huxerui.HuxerUIApplication$* { *; }
-keep class org.huxerui.HuxerUIHttpRequest { *; }
-keep class org.huxerui.HuxerUIFilePicker { *; }
-keep class org.huxerui.HuxerUIFilePicker$* { *; }
-keep class org.huxerui.HuxerUIFileReference { *; }
-keep class org.huxerui.HuxerUIFileReference$* { *; }
-keep class org.huxerui.HuxerUIPlatformChannel { *; }
-keep class org.huxerui.HuxerUIPlatformChannel$* { *; }
-keep class org.huxerui.HuxerUIExternalTexture { *; }
-keep class org.huxerui.HuxerUIBufferReference { *; }
-keep class org.huxerui.HuxerUIBufferReference$* { *; }
-keep class org.huxerui.PlatformPayload { *; }
-keep class org.huxerui.PlatformPayload$* { *; }
-keep class * implements org.huxerui.HuxerUIPlatformModule$Factory { public <init>(); *; }
-keep class * implements org.huxerui.HuxerUIPlatformView$Factory { public <init>(); *; }
-keepclassmembers class org.huxerui.HuxerUIView {
    int nativeBackdropAvailable(android.graphics.Canvas);
    int nativeBackdropFallback(java.lang.Throwable);
    android.graphics.Canvas beginNativeBackdropCanvas(android.graphics.Canvas);
    void drawNativeBackdrop(android.graphics.Canvas, float[], float[]);
    java.lang.String backdropRasterEnvironment();
    void redrawBackdropHardware(android.graphics.Canvas);
    boolean backdropGpuAvailable(android.graphics.Canvas);
    android.graphics.Canvas beginBackdropGpuCanvas(android.graphics.Canvas);
    java.lang.Object backdropHardwareRaster(android.graphics.Canvas, int, int, int, int);
    android.graphics.Bitmap backdropBitmap(android.graphics.Canvas);
    android.graphics.Bitmap backdropMask(android.graphics.Canvas, float[]);
    void clearBackdropRaster(android.graphics.Canvas);
    void discardBackdropCanvas(android.graphics.Canvas);
    void presentBackdropHardware(android.graphics.Canvas, java.lang.Object);
    void setBackdropRasterTransform(android.graphics.Canvas, float, float, float, float, float, float);
    boolean canOpenFiles();
    boolean canSaveFiles();
    org.huxerui.HuxerUIFilePicker$Operation prepareOpenDirectory(long, boolean);
    org.huxerui.HuxerUIFilePicker$Operation prepareOpenFiles(long, java.lang.String[], java.lang.String[], boolean);
    org.huxerui.HuxerUIFilePicker$Operation prepareSaveFile(long, java.lang.String, java.lang.String, java.lang.String[], java.lang.String[]);
}
