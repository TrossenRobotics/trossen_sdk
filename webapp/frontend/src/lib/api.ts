/**
 * Centralised HTTP client for the Trossen SDK webapp.
 *
 * Wraps `fetch` so every API call shares the same error story, request
 * shape, and response parsing. All callers (pages, hooks, tests) should
 * go through this module instead of calling `fetch` directly — that way
 * a change here (auth header, telemetry, retry policy) lands in one
 * place instead of a hundred call sites.
 *
 * What this module deliberately does NOT do (yet):
 *   - No automatic retries.        Add a `retries` option to
 *                                  ApiRequestOptions if you need them
 *                                  (e.g. for idempotent GETs over a
 *                                  flaky network).
 *   - No caching/deduplication.    Pair with a hook like SWR or
 *                                  React Query, or extend `useApiFetch`.
 *   - No auth headers.             When auth lands, either inject from
 *                                  a single helper here (e.g.
 *                                  `getAuthHeader()`) or pass per-call
 *                                  via `opts.headers`.
 *   - No request/response logging. Add a thin wrapper around `fetch`
 *                                  here if you want central telemetry.
 *
 * Promote to a fuller HTTP client (axios / ky / @tanstack/react-query)
 * only if you feel pain from one of the above. Otherwise this shim stays
 * small, predictable, and fast to read.
 */

/**
 * Thrown when the server responds with a non-2xx status.
 *
 * Carries:
 *   - status: HTTP status number (use to branch on 401, 404, 500, ...)
 *   - detail: parsed body. For FastAPI errors that's the inner `detail`
 *             field; for everything else it's the whole parsed body.
 *
 * Future ideas:
 *   - Add a `code` field if you adopt a structured error format
 *     ({detail: {code: "VALIDATION_FAILED"}}) so UI can branch on
 *     machine-readable codes instead of message strings.
 */
export class ApiError extends Error {
    readonly status: number;
    readonly detail: unknown;

    constructor(status: number, detail: unknown, message?: string) {
        super(message ?? `HTTP ${status}`);
        this.name = 'ApiError';
        this.status = status;
        this.detail = detail;
    }
}

/**
 * Thrown when the request couldn't reach the server at all (DNS failure,
 * server down, CORS blocked). Distinct from ApiError — the request
 * never got a response.
 *
 * Future ideas:
 *   - Check `navigator.onLine` before throwing to distinguish
 *     "offline" from "server down" and surface different UX.
 */
export class NetworkError extends Error {
    constructor(message = 'Network request failed') {
        super(message);
        this.name = 'NetworkError';
    }
}

/**
 * Per-call options. All fields optional.
 *
 *   body    — JSON-serialisable payload. Content-Type set automatically.
 *   signal  — pass an AbortController.signal to cancel mid-flight.
 *   headers — extra headers, merged with the defaults.
 *
 * Future additions to consider:
 *   - timeoutMs?: number    Auto-abort after N ms
 *                           (`AbortSignal.timeout(ms)`).
 *   - retries?: number      Retry-with-backoff for idempotent calls.
 *   - skipJsonParse?: bool  For binary / stream responses.
 *   - credentials?: ...     When the backend needs cookies, set
 *                           credentials: 'include'.
 */
export interface ApiRequestOptions {
    body?: unknown;
    signal?: AbortSignal;
    headers?: Record<string, string>;
}

/**
 * Restrict callers to verbs we actually use. Add new verbs here when
 * the backend grows new endpoint shapes (e.g. HEAD, OPTIONS).
 */
type HttpMethod = 'GET' | 'POST' | 'PUT' | 'DELETE' | 'PATCH';

/**
 * Issue an API request. Returns parsed JSON typed as `T`, or `undefined`
 * for 204 responses.
 *
 * Throws (in priority order):
 *   - DOMException("AbortError") — caller cancelled via opts.signal
 *   - NetworkError               — couldn't reach the server
 *   - ApiError                   — server replied with non-2xx
 *
 * Usage:
 *   const sessions = await apiRequest<Session[]>('GET', '/api/sessions');
 *
 * Most code uses the apiGet / apiPost / apiPut / apiDelete shortcuts
 * below instead of calling apiRequest directly.
 */
export async function apiRequest<T = unknown>(
    method: HttpMethod,
    path: string,
    opts: ApiRequestOptions = {},
): Promise<T> {
    // Build headers up from caller-provided ones; we'll add Content-Type
    // below if there's a body. Spread into a fresh object so we don't
    // mutate the caller's headers.
    const headers: Record<string, string> = { ...(opts.headers ?? {}) };

    // Serialise the body if present. We only support JSON today; if you
    // need form-encoded data (multipart, urlencoded), branch here on the
    // body type and set the appropriate Content-Type.
    let body: BodyInit | undefined;
    if (opts.body !== undefined) {
        body = JSON.stringify(opts.body);
        headers['Content-Type'] = 'application/json';
    }

    // fetch() throws only on network failure or explicit abort — non-2xx
    // responses are NOT exceptions, you check res.ok / res.status to
    // detect them. That's why we have two distinct error classifications.
    let res: Response;
    try {
        res = await fetch(path, { method, headers, body, signal: opts.signal });
    } catch (err) {
        // Aborts are intentional cancellations (e.g. useApiFetch when a
        // component unmounts). Rethrow unchanged so callers can ignore the
        // noise.
        if (err instanceof DOMException && err.name === 'AbortError') throw err;
        throw new NetworkError(err instanceof Error ? err.message : String(err));
    }

    // 204 No Content has no body. Return undefined; callers asserting a
    // non-undefined T are responsible for not pointing at 204 endpoints.
    if (res.status === 204) return undefined as T;

    // Read body as text first, then decide how to handle it. Splitting the
    // success and error paths matters: error responses can legitimately be
    // non-JSON (HTML 500 pages, plain-text proxy errors), but a 2xx with
    // non-JSON content is a contract violation worth surfacing — otherwise
    // callers receive raw HTML typed as T and crash later (e.g. when they
    // try to call `.filter()` on what they thought was an array).
    const text = await res.text();

    if (!res.ok) {
        // Error path: try JSON for FastAPI-style {detail: ...}; fall back to
        // the raw text so the user still sees something useful.
        let parsed: unknown = text;
        if (text) {
            try { parsed = JSON.parse(text); } catch { /* keep raw text */ }
        }
        const detailField =
            parsed && typeof parsed === 'object' && 'detail' in parsed
                ? (parsed as { detail: unknown }).detail
                : parsed;
        const message =
            typeof detailField === 'string' ? detailField : `HTTP ${res.status}`;
        throw new ApiError(res.status, detailField, message);
    }

    // Success path: empty body → undefined; otherwise must be valid JSON.
    // The `as T` is a deliberate trust boundary — TS can't validate the
    // server's response shape at runtime. For high-assurance endpoints,
    // validate with zod or similar at the call site.
    if (!text) return undefined as T;
    try {
        return JSON.parse(text) as T;
    } catch {
        throw new ApiError(
            res.status,
            text,
            `Expected JSON from ${path}, got non-JSON response`,
        );
    }
}

// ---------------------------------------------------------------------------
// Method shortcuts.
//
// Thin sugar over apiRequest. Each one fixes the HTTP method so call sites
// read like `apiGet('/api/foo')` instead of the wordier
// `apiRequest('GET', '/api/foo')`.
//
// Add new shortcuts here if you ever need PATCH, HEAD, or other verbs.
// ---------------------------------------------------------------------------

export const apiGet = <T = unknown>(path: string, opts?: ApiRequestOptions) =>
    apiRequest<T>('GET', path, opts);

export const apiPost = <T = unknown>(
    path: string,
    body?: unknown,
    opts?: ApiRequestOptions,
) => apiRequest<T>('POST', path, { ...opts, body });

export const apiPut = <T = unknown>(
    path: string,
    body?: unknown,
    opts?: ApiRequestOptions,
) => apiRequest<T>('PUT', path, { ...opts, body });

export const apiDelete = <T = unknown>(
    path: string,
    opts?: ApiRequestOptions,
) => apiRequest<T>('DELETE', path, opts);

/**
 * Turn any thrown value into a user-facing string. Intended for toast
 * messages and inline error UIs where the exact type doesn't matter,
 * only the text shown to the user.
 *
 * Order of `instanceof` checks matters: ApiError extends Error, so the
 * ApiError branch must come before the Error fallback.
 *
 * Future ideas:
 *   - Localise messages with i18n.
 *   - Map specific status codes to friendlier copy
 *     (e.g. 401 → "Please sign in again").
 */
export function describeError(err: unknown): string {
    if (err instanceof ApiError) return err.message;
    if (err instanceof NetworkError) return 'Cannot reach the server';
    if (err instanceof Error) return err.message;
    return String(err);
}
