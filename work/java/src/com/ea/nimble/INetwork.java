package com.ea.nimble;

/**
 * Network status for the game's native Nimble bridge. Kept because the bridge turns {@code getStatus()} into its own
 * enum with {@code Enum.ordinal()} and does not check for a missing component there. The HTTP methods of the
 * original interface are gone with the online services.
 */
public interface INetwork {
    Network.Status getStatus();

    boolean isNetworkWifi();

    void forceRedetectNetworkStatus();
}
