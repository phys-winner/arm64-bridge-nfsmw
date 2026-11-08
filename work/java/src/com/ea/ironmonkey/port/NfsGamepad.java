package com.ea.ironmonkey.port;

import android.app.Activity;
import android.content.Context;
import android.hardware.input.InputManager;
import android.opengl.GLSurfaceView;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

import com.bda.controller.ControllerListener;
import com.bda.controller.StateEvent;

/**
 * Gamepad support (stage 15), ported from the OUYA build 1.1.31 (com.ea.ironmonkey.GamepadGP02): any Android
 * gamepad, Bluetooth or USB (Xbox, DualShock/DualSense, 8BitDo and other HID pads), drives the game.
 *
 * The OUYA build hands the buttons and the stick axes of android.view events to natives of its own on the GL
 * thread. The 1.3.128 engine has no such natives, but it has a complete MOGA controller path with the same action
 * table (libapp 0x5449A0 here, 0x6D8C70 in the OUYA build): ControllerControlScheme (steering = asin(left stick
 * X), A drift, L1 brake, R1 nitro, right stick up nitro and down brake), menu navigation with highlights, all fed
 * by com.bda.controller events and switched on by Controller.getState(STATE_CONNECTION), which the engine reads on
 * every resume. That path needs the MOGA Pivot service, so it never ran. This class stands in for the service:
 * Controller.getState and Controller.setListener (bda smali) call in here, and GameActivityMain hands over the
 * gamepad's key and joystick events, which reach the engine's MogaController listener as com.bda.controller events
 * on the GL thread, as the game's own key events do.
 *
 * A D-pad reported as hat axes and analog triggers become D-pad and L2/R2 buttons; L2/R2 act as L1/R1 (the engine
 * maps them so). The right stick is reported only at its ends (OUYA threshold 0.7, released below 0.3): the engine
 * acts on MOGA's Z/RZ only at exactly -1 and 1 and repeats the press on every event while a value stays there.
 */
public final class NfsGamepad {
    static final String TAG = "NfsGamepad";
    /** GameActivityMain.state while the game loop runs; its onKeyDown forwards keys to the engine only then. */
    private static final int RUNNING = 8;
    // com.bda.controller.Controller
    private static final int CONTROLLER_ID = 1;
    private static final int STATE_SUPPORTED_PRODUCT_VERSION = 3;
    private static final int STATE_CURRENT_PRODUCT_VERSION = 4;
    private static final int ACTION_VERSION_MOGAPRO = 1;

    private static final float DEADZONE = 0.12f;
    private static final float END_FIRE = 0.7f;
    private static final float END_LEAVE = 0.3f;
    /** Z/RZ while the right stick stays at an end: not 0 and not an end, so the engine only takes the left stick. */
    private static final float END_HELD = 0.5f;
    private static final int NONE = 0, LEFT = 1, RIGHT = 2, UP = 3, DOWN = 4;
    // What holds a MOGA key: the button itself, L2/R2 for L1/R1, or a hat or trigger axis
    private static final int BY_KEY = 1, BY_SECOND_KEY = 2, BY_AXIS = 4;

    private static volatile ControllerListener sListener;
    private static volatile GLSurfaceView sView;
    private static boolean sConnected;
    // Main thread only (dispatch*Event, pause, device changes)
    private static final int[] sHeld = new int[256];
    private static final boolean[] sPressSent = new boolean[256];
    private static int sEnd = NONE;
    private static float sX, sY;
    private static boolean sStickSent;

    private NfsGamepad() {}

    private static final InputManager.InputDeviceListener DEVICES = new InputManager.InputDeviceListener() {
        @Override
        public void onInputDeviceAdded(int deviceId) {
            devicesChanged();
        }

        @Override
        public void onInputDeviceRemoved(int deviceId) {
            devicesChanged();
        }

        @Override
        public void onInputDeviceChanged(int deviceId) {
            devicesChanged();
        }
    };

    /** GameActivityMain.onCreate, once the GL view exists. */
    public static void attach(Activity activity, GLSurfaceView view) {
        sView = view;
        InputManager input = (InputManager) activity.getSystemService(Context.INPUT_SERVICE);
        if (input != null) input.registerInputDeviceListener(DEVICES, new Handler(Looper.getMainLooper()));
        String pads = gamepads();
        synchronized (NfsGamepad.class) {
            sConnected = !pads.isEmpty();
        }
        if (!pads.isEmpty()) NfsLog.i(TAG, "gamepad present: " + pads);
    }

    /** Controller.setListener (bda smali): the engine's MogaController, or null on Controller.exit(). */
    public static void setListener(ControllerListener listener) {
        sListener = listener;
    }

    /** Controller.getState (bda smali) while no MOGA service is bound. */
    public static int mogaState(int state) {
        switch (state) {
            case StateEvent.STATE_CONNECTION: {
                boolean connected = !gamepads().isEmpty();
                synchronized (NfsGamepad.class) {
                    sConnected = connected;
                }
                return connected ? StateEvent.ACTION_CONNECTED : StateEvent.ACTION_DISCONNECTED;
            }
            case STATE_SUPPORTED_PRODUCT_VERSION:
            case STATE_CURRENT_PRODUCT_VERSION:
                return ACTION_VERSION_MOGAPRO;
            default:
                return 0;
        }
    }

    /** GameActivityMain.dispatchKeyEvent, before the view and onKeyDown: true when the key was a gamepad's. */
    public static boolean onKeyEvent(KeyEvent event, int gameState) {
        int code = event.getKeyCode();
        if (KeyEvent.isGamepadButton(code)) {
            if (code == KeyEvent.KEYCODE_BUTTON_MODE) return false;  // the system's (home on some devices)
        } else if (code < KeyEvent.KEYCODE_DPAD_UP || code > KeyEvent.KEYCODE_DPAD_RIGHT || !fromGamepad(event)) {
            return false;  // Back, volume and keyboard keys keep their usual way
        }
        int action = event.getAction();
        if ((action != KeyEvent.ACTION_DOWN && action != KeyEvent.ACTION_UP) || event.getRepeatCount() > 0) return true;
        int moga = mogaKey(code);
        if (moga != 0) {
            boolean second = code == KeyEvent.KEYCODE_BUTTON_L2 || code == KeyEvent.KEYCODE_BUTTON_R2;
            hold(moga, second ? BY_SECOND_KEY : BY_KEY, action == KeyEvent.ACTION_DOWN, event.getEventTime(), gameState);
        }
        return true;
    }

    /**
     * GameActivityMain.dispatchGenericMotionEvent: true for joystick events. They are consumed, so Android does not
     * turn the sticks and the hat into D-pad keys of its own.
     */
    public static boolean onMotionEvent(MotionEvent event, int gameState) {
        if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK) return false;
        if (event.getAction() != MotionEvent.ACTION_MOVE) return true;
        InputDevice device = event.getDevice();
        long time = event.getEventTime();
        float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
        float hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        hold(KeyEvent.KEYCODE_DPAD_LEFT, BY_AXIS, hatX < -0.5f, time, gameState);
        hold(KeyEvent.KEYCODE_DPAD_RIGHT, BY_AXIS, hatX > 0.5f, time, gameState);
        hold(KeyEvent.KEYCODE_DPAD_UP, BY_AXIS, hatY < -0.5f, time, gameState);
        hold(KeyEvent.KEYCODE_DPAD_DOWN, BY_AXIS, hatY > 0.5f, time, gameState);
        trigger(KeyEvent.KEYCODE_BUTTON_L1, Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER),
                event.getAxisValue(MotionEvent.AXIS_BRAKE)), time, gameState);
        trigger(KeyEvent.KEYCODE_BUTTON_R1, Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
                event.getAxisValue(MotionEvent.AXIS_GAS)), time, gameState);
        // The right stick is Z/RZ on most pads; some (the OUYA build's GP02 among them) report it as RX/RY
        boolean rxry = device != null && device.getMotionRange(MotionEvent.AXIS_RZ, event.getSource()) == null
                && device.getMotionRange(MotionEvent.AXIS_RY, event.getSource()) != null;
        stick(centred(event, device, MotionEvent.AXIS_X), centred(event, device, MotionEvent.AXIS_Y),
                event.getAxisValue(rxry ? MotionEvent.AXIS_RX : MotionEvent.AXIS_Z),
                event.getAxisValue(rxry ? MotionEvent.AXIS_RY : MotionEvent.AXIS_RZ), time);
        return true;
    }

    /** GameActivityMain.onPause: a button or stick held while the game goes away must not stay held in the engine. */
    public static void pause() {
        release(SystemClock.uptimeMillis());
    }

    private static void devicesChanged() {
        String pads = gamepads();
        boolean connected = !pads.isEmpty();
        synchronized (NfsGamepad.class) {
            if (connected == sConnected) return;
            sConnected = connected;
        }
        NfsLog.i(TAG, connected ? "gamepad connected: " + pads : "gamepad disconnected");
        long now = SystemClock.uptimeMillis();
        if (!connected) release(now);
        post(new StateEvent(now, CONTROLLER_ID, StateEvent.STATE_CONNECTION,
                connected ? StateEvent.ACTION_CONNECTED : StateEvent.ACTION_DISCONNECTED));
    }

    /** Names of the attached gamepads (joystick devices with a stick), empty when there is none. */
    private static String gamepads() {
        StringBuilder names = new StringBuilder();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device == null || device.isVirtual()
                    || (device.getSources() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK
                    || device.getMotionRange(MotionEvent.AXIS_X, InputDevice.SOURCE_JOYSTICK) == null) {
                continue;
            }
            if (names.length() > 0) names.append(", ");
            names.append(device.getName()).append(" (").append(Integer.toHexString(device.getVendorId())).append(':')
                    .append(Integer.toHexString(device.getProductId())).append(')');
        }
        return names.toString();
    }

    private static boolean fromGamepad(KeyEvent event) {
        int source = event.getSource();
        if ((source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            return true;
        }
        InputDevice device = event.getDevice();
        return device != null && (device.getSources() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD;
    }

    /** The MOGA key a gamepad key drives, or 0 for keys the engine does not use (thumb, C, Z, numbered buttons). */
    private static int mogaKey(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_UP:
            case KeyEvent.KEYCODE_DPAD_DOWN:
            case KeyEvent.KEYCODE_DPAD_LEFT:
            case KeyEvent.KEYCODE_DPAD_RIGHT:
            case KeyEvent.KEYCODE_BUTTON_A:
            case KeyEvent.KEYCODE_BUTTON_B:
            case KeyEvent.KEYCODE_BUTTON_X:
            case KeyEvent.KEYCODE_BUTTON_Y:
            case KeyEvent.KEYCODE_BUTTON_L1:
            case KeyEvent.KEYCODE_BUTTON_R1:
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_BUTTON_SELECT:
                return code;
            case KeyEvent.KEYCODE_BUTTON_L2:
                return KeyEvent.KEYCODE_BUTTON_L1;
            case KeyEvent.KEYCODE_BUTTON_R2:
                return KeyEvent.KEYCODE_BUTTON_R1;
            default:
                return 0;
        }
    }

    private static void trigger(int moga, float value, long time, int gameState) {
        boolean held = (sHeld[moga] & BY_AXIS) != 0;
        hold(moga, BY_AXIS, value > (held ? 0.3f : 0.5f), time, gameState);
    }

    /** One input of a MOGA key changed; the engine sees the key's transitions (a press only while the game runs). */
    private static void hold(int moga, int by, boolean down, long time, int gameState) {
        int before = sHeld[moga];
        int after = down ? before | by : before & ~by;
        sHeld[moga] = after;
        if (before == 0 && after != 0 && gameState == RUNNING) {
            sPressSent[moga] = true;
            post(new com.bda.controller.KeyEvent(time, CONTROLLER_ID, moga, com.bda.controller.KeyEvent.ACTION_DOWN));
        } else if (before != 0 && after == 0 && sPressSent[moga]) {
            sPressSent[moga] = false;
            post(new com.bda.controller.KeyEvent(time, CONTROLLER_ID, moga, com.bda.controller.KeyEvent.ACTION_UP));
        }
    }

    private static float centred(MotionEvent event, InputDevice device, int axis) {
        float value = event.getAxisValue(axis);
        InputDevice.MotionRange range = device != null ? device.getMotionRange(axis, event.getSource()) : null;
        float flat = Math.max(DEADZONE, range != null ? Math.min(range.getFlat(), 0.5f) : 0f);
        float magnitude = Math.abs(value);
        if (magnitude <= flat) return 0f;
        return Math.copySign(Math.min(1f, (magnitude - flat) / (1f - flat)), value);
    }

    /** Right stick end with hysteresis: an end holds until the stick is back below END_LEAVE on its axis. */
    private static int end(float rx, float ry) {
        switch (sEnd) {
            case LEFT:
                if (rx < -END_LEAVE) return LEFT;
                break;
            case RIGHT:
                if (rx > END_LEAVE) return RIGHT;
                break;
            case UP:
                if (ry < -END_LEAVE) return UP;
                break;
            case DOWN:
                if (ry > END_LEAVE) return DOWN;
                break;
            default:
                break;
        }
        if (Math.abs(rx) >= Math.abs(ry)) return rx < -END_FIRE ? LEFT : rx > END_FIRE ? RIGHT : NONE;
        return ry < -END_FIRE ? UP : ry > END_FIRE ? DOWN : NONE;
    }

    /**
     * Left stick and right stick end as one MOGA motion event (libapp 0x2667A0): X/Y are stored for steering on every
     * event; Z = -1/1 is a tap of the right stick left/right (menus), RZ = -1 nitro and RZ = 1 brake, held until RZ is
     * 0 again (a race). An end is sent once; while it is held Z/RZ carry END_HELD.
     */
    private static void stick(float x, float y, float rx, float ry, long time) {
        int end = end(rx, ry);
        if (end == sEnd && sStickSent && Math.abs(x - sX) < 0.004f && Math.abs(y - sY) < 0.004f) return;
        if (sEnd == DOWN && end != DOWN && end != NONE) send(x, y, 0f, 0f, time);  // release the brake first
        if (end == NONE) send(x, y, 0f, 0f, time);
        else if (end != sEnd) send(x, y, end == LEFT ? -1f : end == RIGHT ? 1f : 0f, end == UP ? -1f : end == DOWN ? 1f : 0f, time);
        else send(x, y, END_HELD, END_HELD, time);
        sEnd = end;
        sX = x;
        sY = y;
        sStickSent = true;
    }

    private static void send(float x, float y, float z, float rz, long time) {
        post(new com.bda.controller.MotionEvent(time, CONTROLLER_ID, x, y, z, rz, 0f, 0f));
    }

    /** Every held key up and both sticks centred (pause, gamepad gone). */
    private static void release(long time) {
        for (int moga = 0; moga < sHeld.length; moga++) {
            if (sHeld[moga] == 0) continue;
            sHeld[moga] = 0;
            if (sPressSent[moga]) {
                sPressSent[moga] = false;
                post(new com.bda.controller.KeyEvent(time, CONTROLLER_ID, moga, com.bda.controller.KeyEvent.ACTION_UP));
            }
        }
        if (sStickSent) send(0f, 0f, 0f, 0f, time);
        sEnd = NONE;
        sX = 0f;
        sY = 0f;
        sStickSent = false;
    }

    /** Runs the listener on the GL thread, where the engine handles its other input. */
    private static void post(final Object event) {
        final ControllerListener listener = sListener;
        GLSurfaceView view = sView;
        if (listener == null || view == null) return;
        view.queueEvent(new Runnable() {
            @Override
            public void run() {
                if (event instanceof com.bda.controller.KeyEvent) listener.onKeyEvent((com.bda.controller.KeyEvent) event);
                else if (event instanceof com.bda.controller.MotionEvent) listener.onMotionEvent((com.bda.controller.MotionEvent) event);
                else listener.onStateEvent((StateEvent) event);
            }
        });
    }
}
