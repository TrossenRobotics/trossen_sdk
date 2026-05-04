/**
 * WebSocket hook with reconnect support.
 *
 * Opens a connection to `url` while `enabled` is true, forwards events to
 * the caller's handlers, and reconnects with exponential backoff (1s →
 * 30s) on unexpected disconnects. A clean close (code 1000) does not
 * trigger a retry.
 *
 * The handler functions (`onMessage`, `onOpen`, `onClose`) are tracked
 * via refs so that updating them does not tear down and re-establish the
 * socket — only `url` and `enabled` changes do.
 */

import { useEffect, useRef, useState } from 'react';

export type WsStatus = 'open' | 'connecting' | 'reconnecting' | 'closed';

export interface UseReconnectingWebSocketOptions {
    url: string | null;
    enabled: boolean;
    onMessage?: (event: MessageEvent) => void;
    onOpen?: (event: Event) => void;
    onClose?: (event: CloseEvent) => void;
}

export interface UseReconnectingWebSocketResult {
    status: WsStatus;
}

const INITIAL_BACKOFF_MS = 1000;
const MAX_BACKOFF_MS = 30000;

export function useReconnectingWebSocket(
    opts: UseReconnectingWebSocketOptions,
): UseReconnectingWebSocketResult {
    const { url, enabled, onMessage, onOpen, onClose } = opts;
    const [status, setStatus] = useState<WsStatus>('closed');

    // Refs so handler identity doesn't tear down the socket every render.
    const onMessageRef = useRef(onMessage);
    const onOpenRef = useRef(onOpen);
    const onCloseRef = useRef(onClose);
    useEffect(() => { onMessageRef.current = onMessage; }, [onMessage]);
    useEffect(() => { onOpenRef.current = onOpen; }, [onOpen]);
    useEffect(() => { onCloseRef.current = onClose; }, [onClose]);

    useEffect(() => {
        if (!enabled || !url) {
            setStatus('closed');
            return;
        }

        let socket: WebSocket | null = null;
        let backoffMs = INITIAL_BACKOFF_MS;
        let retryTimer: ReturnType<typeof setTimeout> | null = null;
        let cancelled = false;

        const connect = () => {
            if (cancelled) return;
            // First attempt is `connecting`; subsequent ones are `reconnecting`.
            setStatus(prev => (prev === 'closed' ? 'connecting' : 'reconnecting'));

            const ws = new WebSocket(url);
            socket = ws;

            ws.addEventListener('open', (event) => {
                if (cancelled) {
                    ws.close();
                    return;
                }
                backoffMs = INITIAL_BACKOFF_MS;
                setStatus('open');
                onOpenRef.current?.(event);
            });

            ws.addEventListener('message', (event) => {
                if (cancelled) return;
                onMessageRef.current?.(event);
            });

            ws.addEventListener('close', (event) => {
                if (cancelled) return;
                onCloseRef.current?.(event);
                if (event.code === 1000) {
                    // Clean close — don't reconnect.
                    setStatus('closed');
                    return;
                }
                setStatus('reconnecting');
                retryTimer = setTimeout(connect, backoffMs);
                backoffMs = Math.min(backoffMs * 2, MAX_BACKOFF_MS);
            });

            // The 'error' event fires before 'close' and carries no useful
            // detail; we let the close handler drive reconnect logic.
        };

        connect();

        return () => {
            cancelled = true;
            if (retryTimer) clearTimeout(retryTimer);
            if (socket) {
                // Suppress the close handler's reconnect path via `cancelled`.
                if (socket.readyState === WebSocket.OPEN ||
                    socket.readyState === WebSocket.CONNECTING) {
                    socket.close(1000, 'unmount');
                }
            }
        };
    }, [url, enabled]);

    return { status };
}
