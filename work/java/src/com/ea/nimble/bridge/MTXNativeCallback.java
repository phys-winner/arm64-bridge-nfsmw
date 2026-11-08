package com.ea.nimble.bridge;

import com.ea.nimble.mtx.INimbleMTX;
import com.ea.nimble.mtx.NimbleMTXTransaction;

/**
 * MTX callbacks created by the native NimbleWrapper ({@code <init>(I)V}); each forwards the transaction to the native
 * callback id with the original argument layout (purchaseComplete: [transaction, true], unverifiedReceiptReceived:
 * [transaction, false], itemGrantedComplete and finalizeComplete: [transaction]).
 */
public class MTXNativeCallback implements INimbleMTX.PurchaseTransactionCallback, INimbleMTX.ItemGrantedCallback,
        INimbleMTX.FinalizeTransactionCallback {
    private final int m_id;

    public MTXNativeCallback(int id) {
        m_id = id;
    }

    @Override
    public void purchaseComplete(NimbleMTXTransaction transaction) {
        BaseNativeCallback.sendNativeCallback(m_id, transaction, Boolean.TRUE);
    }

    @Override
    public void unverifiedReceiptReceived(NimbleMTXTransaction transaction) {
        BaseNativeCallback.sendNativeCallback(m_id, transaction, Boolean.FALSE);
    }

    @Override
    public void itemGrantedComplete(NimbleMTXTransaction transaction) {
        BaseNativeCallback.sendNativeCallback(m_id, transaction);
    }

    @Override
    public void finalizeComplete(NimbleMTXTransaction transaction) {
        BaseNativeCallback.sendNativeCallback(m_id, transaction);
    }

    @Override
    protected void finalize() throws Throwable {
        super.finalize();
        BaseNativeCallback.finalizeLater(m_id);
    }
}
