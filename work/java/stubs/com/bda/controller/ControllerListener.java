package com.bda.controller;

/**
 * Compile-time stub of the MOGA SDK interface the game ships in app/smali (stage 15). build_port_code.py puts
 * work/java/stubs on the javac class path only; nothing from here is packaged, the app's own classes are used.
 */
public interface ControllerListener {
    void onKeyEvent(KeyEvent event);

    void onMotionEvent(MotionEvent event);

    void onStateEvent(StateEvent event);
}
