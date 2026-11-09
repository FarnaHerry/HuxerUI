package org.huxerui.examples;

import org.huxerui.HuxerUIActivity;

public final class MainActivity extends HuxerUIActivity {
    static {
        System.setProperty("org.huxerui.experimental.nativeBlur", BuildConfig.HUXERUI_NATIVE_BLUR);
    }
}
