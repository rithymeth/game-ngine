package com.aether.engine;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public final class MainActivity extends Activity {
    static {
        System.loadLibrary("aether_android");
    }

    private static native String nativeEngineStatus();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView status = new TextView(this);
        status.setText(nativeEngineStatus());
        status.setTextSize(18);
        status.setPadding(32, 32, 32, 32);
        setContentView(status);
    }
}
