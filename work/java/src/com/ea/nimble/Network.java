package com.ea.nimble;

/**
 * Offline network component: always "no network". The original NetworkImpl reported NONE without a connection.
 * This build has no INTERNET permission and EA's servers are shut down.
 */
public final class Network {
    /** Original order: the native bridge uses the ordinal (UNKNOWN 0, NONE 1, DEAD 2, OK 3). */
    public enum Status { UNKNOWN, NONE, DEAD, OK }

    private static final INetwork OFFLINE = new INetwork() {
        @Override
        public Status getStatus() {
            return Status.NONE;
        }

        @Override
        public boolean isNetworkWifi() {
            return false;
        }

        @Override
        public void forceRedetectNetworkStatus() {}
    };

    private Network() {}

    public static INetwork getComponent() {
        return OFFLINE;
    }
}
