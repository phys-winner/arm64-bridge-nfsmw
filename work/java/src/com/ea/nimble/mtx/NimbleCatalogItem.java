package com.ea.nimble.mtx;

import java.util.HashMap;
import java.util.Map;

/**
 * A store item with the eight getters the native NimbleWrapper reads (MTX catalog -> StoreScreen cards). The SKU is the
 * item's SellID; StoreScreen maps SellIDs to its cash packs (StoreScreen_GetItems_Callback_SellIdMap 0x19BACC).
 */
public final class NimbleCatalogItem {
    /** Original order; the native bridge reads these constants as static fields. */
    public enum ItemType { UNKNOWN, NONCONSUMABLE, CONSUMABLE, SUBSCRIPTION }

    private final String m_sku;
    private final String m_title;
    private final String m_description;
    private final float m_price;
    private final String m_priceText;
    private final ItemType m_type;

    public NimbleCatalogItem(String sku, String title, String description, float price, String priceText, ItemType type) {
        m_sku = sku;
        m_title = title;
        m_description = description;
        m_price = price;
        m_priceText = priceText;
        m_type = type;
    }

    public String getSku() {
        return m_sku;
    }

    public String getTitle() {
        return m_title;
    }

    public String getDescription() {
        return m_description;
    }

    public float getPriceDecimal() {
        return m_price;
    }

    public String getPriceWithCurrencyAndFormat() {
        return m_priceText;
    }

    public ItemType getItemType() {
        return m_type;
    }

    public String getMetaDataUrl() {
        return "";
    }

    public Map<String, String> getAdditionalInfo() {
        return new HashMap<>();
    }
}
