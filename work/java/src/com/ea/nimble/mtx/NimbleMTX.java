package com.ea.nimble.mtx;

import com.ea.ironmonkey.port.NfsQaStore;

/** The MTX component the native NimbleWrapper asks for (getComponent()): the restoration QA store. */
public final class NimbleMTX {
    public static final String COMPONENT_ID = "com.ea.nimble.mtx";

    private NimbleMTX() {}

    public static INimbleMTX getComponent() {
        return NfsQaStore.instance();
    }
}
