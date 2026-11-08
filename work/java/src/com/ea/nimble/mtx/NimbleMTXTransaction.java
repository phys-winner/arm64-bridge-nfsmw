package com.ea.nimble.mtx;

import java.util.Date;
import java.util.HashMap;
import java.util.Map;

/** A store transaction with the ten getters the native NimbleWrapper reads. A null getError() means success. */
public final class NimbleMTXTransaction {
    /** Original orders (the native bridge converts by ordinal). */
    public enum TransactionState {
        UNDEFINED, USER_INITIATED, WAITING_FOR_PREPURCHASE_INFO, WAITING_FOR_PLATFORM_RESPONSE, WAITING_FOR_VERIFICATION,
        WAITING_FOR_GAME_TO_CONFIRM_ITEM_GRANT, WAITING_FOR_PLATFORM_CONSUMPTION, COMPLETE
    }

    public enum TransactionType { PURCHASE, RESTORE }

    private final String m_id;
    private final String m_sku;
    private final Date m_time = new Date();
    private volatile TransactionState m_state = TransactionState.USER_INITIATED;
    private volatile Exception m_error;

    public NimbleMTXTransaction(String id, String sku) {
        m_id = id;
        m_sku = sku;
    }

    public void setState(TransactionState state) {
        m_state = state;
    }

    public void setError(Exception error) {
        m_error = error;
    }

    public String getTransactionId() {
        return m_id;
    }

    public String getItemSku() {
        return m_sku;
    }

    public TransactionState getTransactionState() {
        return m_state;
    }

    public TransactionType getTransactionType() {
        return TransactionType.PURCHASE;
    }

    public float getPriceDecimal() {
        return 0f;
    }

    public String getItemSellId() {
        return m_sku;
    }

    public Date getTimeStamp() {
        return m_time;
    }

    public String getReceipt() {
        return "";
    }

    public Map<String, String> getAdditionalInfo() {
        return new HashMap<>();
    }

    public Exception getError() {
        return m_error;
    }
}
