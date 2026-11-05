package com.aether.engine;

import android.app.Activity;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    static {
        System.loadLibrary("aether_android");
    }

    private static native String nativeEngineStatus();
    // The native side renders into the surface on its own thread until nativeSurfaceDestroyed returns.
    private static native void nativeSurfaceChanged(Surface surface, int width, int height);
    private static native void nativeSurfaceDestroyed();
    // action: 0 down, 1 up (or cancelled), 2 move. Coordinates are in surface pixels.
    private static native void nativeTouch(int pointerId, int action, float x, float y);
    // event: 0 suspend, 1 resume, 2 memory pressure (critical: the system is about to kill background apps).
    private static native void nativeLifecycle(int event, boolean critical);
    private static native void nativeSetUserDataDir(String path);
    // Scene mode runs the cooked game copied to <files>/Paks instead of the triangle test.
    private static native void nativeSetSceneMode(boolean scene);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        nativeSetUserDataDir(getFilesDir().getAbsolutePath());
        boolean scene = "scene".equals(getIntent().getStringExtra("mode"));
        if (scene) copyPaks();
        nativeSetSceneMode(scene);
        FrameLayout root = new FrameLayout(this);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        view.setOnTouchListener((v, event) -> {
            forwardTouch(event);
            return true;
        });
        root.addView(view, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                        ViewGroup.LayoutParams.MATCH_PARENT));
        TextView status = new TextView(this);
        status.setText(nativeEngineStatus());
        status.setTextSize(14);
        status.setPadding(24, 24, 24, 24);
        root.addView(status, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                                                          ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(root);
    }

    // The APK's assets/Paks/*.apak to <files>/Paks, where the player runtime mounts them from.
    private void copyPaks() {
        AssetManager assets = getAssets();
        File dir = new File(getFilesDir(), "Paks");
        if (!dir.isDirectory() && !dir.mkdirs()) return;
        try {
            String[] names = assets.list("Paks");
            if (names == null) return;
            byte[] buffer = new byte[1 << 16];
            for (String name : names) {
                if (!name.endsWith(".apak")) continue;
                try (InputStream in = assets.open("Paks/" + name);
                     OutputStream out = new FileOutputStream(new File(dir, name))) {
                    int n;
                    while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
                }
            }
        } catch (IOException e) {
            // Scene mode then reports that no .apak was found.
        }
    }

    private static void forwardTouch(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = event.getActionIndex();
                nativeTouch(event.getPointerId(i), 0, event.getX(i), event.getY(i));
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = event.getActionIndex();
                nativeTouch(event.getPointerId(i), 1, event.getX(i), event.getY(i));
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                for (int i = 0; i < event.getPointerCount(); ++i) {
                    nativeTouch(event.getPointerId(i), 1, event.getX(i), event.getY(i));
                }
                break;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); ++i) {
                    nativeTouch(event.getPointerId(i), 2, event.getX(i), event.getY(i));
                }
                break;
            default:
                break;
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        nativeLifecycle(0, false);
    }

    @Override
    protected void onResume() {
        super.onResume();
        nativeLifecycle(1, false);
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        // The levels are not one scale: UI_HIDDEN (20) and BACKGROUND (40) sit above RUNNING_CRITICAL (15) but only mean
        // the app left the screen, so a plain HOME press used to report "Critical" and trim every texture.
        if (level == TRIM_MEMORY_UI_HIDDEN || level < TRIM_MEMORY_RUNNING_LOW) return;
        nativeLifecycle(2, level == TRIM_MEMORY_RUNNING_CRITICAL || level >= TRIM_MEMORY_COMPLETE);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeSurfaceChanged(holder.getSurface(), width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSurfaceDestroyed();
    }
}
