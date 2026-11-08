package com.ea.nimble.mtx;

import com.ea.nimble.Error;

import java.util.List;
import java.util.Map;

/**
 * Nimble MTX (store) interface with exactly the eleven methods the game's native NimbleWrapper resolves. The live
 * implementation (Google Play billing + EA Synergy catalog) is gone with the services; NimbleMTX.getComponent()
 * returns the restoration QA store (com.ea.ironmonkey.port.NfsQaStore).
 */
public interface INimbleMTX {
    String NIMBLE_NOTIFICATION_MTX_REFRESH_CATALOG_FINISHED = "nimble.notification.mtx.refreshcatalogfinished";
    String NIMBLE_NOTIFICATION_MTX_RESTORE_PURCHASED_TRANSACTIONS_FINISHED = "nimble.notification.mtx.restorepurchasedtransactionsfinished";
    String NIMBLE_NOTIFICATION_MTX_TRANSACTIONS_RECOVERED = "nimble.notification.mtx.transactionsrecovered";

    Error purchaseItem(String sku, PurchaseTransactionCallback callback, String requestData);

    Error itemGranted(String transactionId, NimbleCatalogItem.ItemType itemType, ItemGrantedCallback callback);

    Error finalizeTransaction(String transactionId, FinalizeTransactionCallback callback);

    void restorePurchasedTransactions();

    List<NimbleMTXTransaction> getPurchasedTransactions();

    List<NimbleMTXTransaction> getPendingTransactions();

    List<NimbleMTXTransaction> getRecoveredTransactions();

    Error resumeTransaction(String transactionId, PurchaseTransactionCallback purchase, ItemGrantedCallback granted,
                            FinalizeTransactionCallback finalize);

    void setPlatformParameters(Map<String, String> parameters);

    void refreshAvailableCatalogItems();

    List<NimbleCatalogItem> getAvailableCatalogItems();

    interface PurchaseTransactionCallback {
        void purchaseComplete(NimbleMTXTransaction transaction);

        void unverifiedReceiptReceived(NimbleMTXTransaction transaction);
    }

    interface ItemGrantedCallback {
        void itemGrantedComplete(NimbleMTXTransaction transaction);
    }

    interface FinalizeTransactionCallback {
        void finalizeComplete(NimbleMTXTransaction transaction);
    }
}
