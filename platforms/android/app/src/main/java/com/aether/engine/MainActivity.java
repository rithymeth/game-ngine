package com.aether.engine;

import android.app.Activity;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        nativeSetUserDataDir(getFilesDir().getAbsolutePath());
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
        if (level >= TRIM_MEMORY_RUNNING_LOW) {
            nativeLifecycle(2, level >= TRIM_MEMORY_RUNNING_CRITICAL);
        }
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
