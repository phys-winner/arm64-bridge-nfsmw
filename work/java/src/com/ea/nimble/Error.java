package com.ea.nimble;

/**
 * Nimble error as the game's native Nimble bridge reads it: constructor (domain, code, message, cause), getDomain(),
 * getCode() and the static ERROR_DOMAIN field. A null Error means success.
 */
public class Error extends Exception {
    public static final String ERROR_DOMAIN = "NimbleError";

    private final String m_domain;
    private final int m_code;

    public Error(String domain, int code, String message, Throwable cause) {
        super(message, cause);
        m_domain = domain;
        m_code = code;
    }

    public String getDomain() {
        return m_domain;
    }

    public int getCode() {
        return m_code;
    }

    @Override
    public String toString() {
        return "Error " + m_domain + " " + m_code + ": " + getMessage();
    }
}
