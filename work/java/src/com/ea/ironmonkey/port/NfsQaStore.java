package com.ea.ironmonkey.port;

import android.os.Handler;
import android.os.Looper;

import com.ea.nimble.Error;
import com.ea.nimble.Utility;
import com.ea.nimble.mtx.INimbleMTX;
import com.ea.nimble.mtx.NimbleCatalogItem;
import com.ea.nimble.mtx.NimbleMTXTransaction;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

/**
 * Restoration QA store behind the game's own store screen (stage 9). EA's catalog server and Google Play billing are
 * shut down, so this MTX component offers the store's five cash packs as local QA items: the game draws its original
 * cards for them (StoreScreen maps the SKU = SellID to its pack table, 0x19BACC) and grants the cash through its own
 * path (StoreScreen_OnUpdate_Grant → ChangeSet_AddCash → save) once the transaction completes. Nothing is charged: the
 * price reads "FREE", each item needs an explicit confirmation in the game's language, and a cancel ends the
 * transaction with USER_CANCELED. The call sequence is the one the native NimbleWrapper expects from a real purchase:
 * purchaseComplete (error null) → itemGranted → itemGrantedComplete → finalizeTransaction → finalizeComplete.
 */
public final class NfsQaStore implements INimbleMTX {
    private static final String TAG = "NfsQaStore";
    private static final String MTX_DOMAIN = "NimbleMTXError";
    private static final int USER_CANCELED = 20003, ITEM_UNAVAILABLE = 20007, TRANSACTION_NOT_RESUMABLE = 20009;
    /** SellIDs of the five packs (the first of each group in StoreScreen's table) and the cash they grant. */
    static final String[] SKUS = {"854354", "854356", "854358", "854360", "854362"};
    static final int[] AMOUNTS = {500000, 1000000, 2000000, 4000000, 8000000};

    private static final NfsQaStore INSTANCE = new NfsQaStore();
    private final Handler m_main = new Handler(Looper.getMainLooper());
    private final Map<String, NimbleMTXTransaction> m_open = new HashMap<>();

    private NfsQaStore() {}

    public static NfsQaStore instance() {
        return INSTANCE;
    }

    @Override
    public void refreshAvailableCatalogItems() {
        Map<String, String> result = new HashMap<>();
        result.put("result", "1");
        Utility.sendBroadcast(NIMBLE_NOTIFICATION_MTX_REFRESH_CATALOG_FINISHED, result);
    }

    @Override
    public List<NimbleCatalogItem> getAvailableCatalogItems() {
        List<NimbleCatalogItem> items = new ArrayList<>();
        for (int i = 0; i < SKUS.length; i++)
            items.add(new NimbleCatalogItem(SKUS[i], NfsQaText.packTitle(i),
                    NfsQaText.get(NfsQaText.ITEM_DESC), 0f, NfsQaText.get(NfsQaText.PRICE_FREE),
                    NimbleCatalogItem.ItemType.CONSUMABLE));
        return items;
    }

    @Override
    public Error purchaseItem(String sku, final PurchaseTransactionCallback callback, String requestData) {
        int pack = -1;
        for (int i = 0; i < SKUS.length; i++) if (SKUS[i].equals(sku)) pack = i;
        if (pack < 0 || callback == null) return new Error(MTX_DOMAIN, ITEM_UNAVAILABLE, "not a QA store item: " + sku, null);
        final NimbleMTXTransaction transaction = new NimbleMTXTransaction(UUID.randomUUID().toString(), sku);
        synchronized (m_open) {
            m_open.put(transaction.getTransactionId(), transaction);
        }
        NfsLog.i(TAG, "QA item " + sku + " (" + AMOUNTS[pack] + ") selected, asking for confirmation");
        NfsQaMenu.confirmGrant(NfsQaText.packTitle(pack), AMOUNTS[pack], () -> {
            transaction.setState(NimbleMTXTransaction.TransactionState.WAITING_FOR_GAME_TO_CONFIRM_ITEM_GRANT);
            callback.purchaseComplete(transaction);
        }, () -> {
            transaction.setState(NimbleMTXTransaction.TransactionState.COMPLETE);
            transaction.setError(new Error(MTX_DOMAIN, USER_CANCELED, "cancelled by the player", null));
            callback.purchaseComplete(transaction);
        });
        return null;
    }

    @Override
    public Error itemGranted(String transactionId, NimbleCatalogItem.ItemType itemType, final ItemGrantedCallback callback) {
        final NimbleMTXTransaction transaction;
        synchronized (m_open) {
            transaction = m_open.get(transactionId);
        }
        if (transaction == null) return new Error(MTX_DOMAIN, TRANSACTION_NOT_RESUMABLE, "unknown transaction", null);
        transaction.setState(NimbleMTXTransaction.TransactionState.COMPLETE);
        if (callback != null) m_main.post(() -> callback.itemGrantedComplete(transaction));
        return null;
    }

    @Override
    public Error finalizeTransaction(String transactionId, final FinalizeTransactionCallback callback) {
        final NimbleMTXTransaction transaction;
        synchronized (m_open) {
            transaction = m_open.remove(transactionId);
        }
        if (transaction == null) return new Error(MTX_DOMAIN, TRANSACTION_NOT_RESUMABLE, "unknown transaction", null);
        transaction.setState(NimbleMTXTransaction.TransactionState.COMPLETE);
        NfsLog.i(TAG, "transaction " + transaction.getItemSku() + " finished" + (transaction.getError() == null ? "" : " (cancelled)"));
        if (callback != null) m_main.post(() -> callback.finalizeComplete(transaction));
        return null;
    }

    @Override
    public void restorePurchasedTransactions() {
        Utility.sendBroadcast(NIMBLE_NOTIFICATION_MTX_RESTORE_PURCHASED_TRANSACTIONS_FINISHED, new HashMap<String, String>());
    }

    @Override
    public List<NimbleMTXTransaction> getPurchasedTransactions() {
        return new ArrayList<>();
    }

    @Override
    public List<NimbleMTXTransaction> getPendingTransactions() {
        return new ArrayList<>();
    }

    @Override
    public List<NimbleMTXTransaction> getRecoveredTransactions() {
        return new ArrayList<>();
    }

    @Override
    public Error resumeTransaction(String transactionId, PurchaseTransactionCallback purchase, ItemGrantedCallback granted,
                                  FinalizeTransactionCallback finalize) {
        return new Error(MTX_DOMAIN, TRANSACTION_NOT_RESUMABLE, "QA items are not resumable", null);
    }

    @Override
    public void setPlatformParameters(Map<String, String> parameters) {}
}
