/**
   * Lightweight logger. Today: console.error wrapper. Future: replace
   * with structured logging (Sentry, custom backend endpoint, etc.)
   * keeping the same logError signature.
   */
export interface LogContext {
    [key: string]: unknown;
}

export function logError(message: string, context?: LogContext): void {
    if (context) {
        console.error(message, context);
    } else {
        console.error(message);
    }
}
