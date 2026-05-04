/**
 * App-level modal for alerts and confirmations.
 *
 * A drop-in replacement for the browser's native `alert()` and
 * `confirm()` that matches the dark theme. Two modes, distinguished by
 * whether `onCancel` is provided:
 *
 *   - Alert  (only onConfirm given): one button, e.g. "OK".
 *   - Confirm (both handlers given): Cancel + Confirm buttons.
 *
 * Behaviour:
 *   - Backdrop click dismisses by calling `onCancel` (confirm mode).
 *     Alert mode ignores backdrop clicks so users must acknowledge.
 *   - Escape key behaves the same as backdrop click.
 *   - Confirm button receives focus on mount so Enter activates it.
 *
 * Future ideas:
 *   - Promise-based imperative API (e.g. `await modal.confirm(...)`)
 *     so callers don't have to manage useState manually.
 *   - Focus trap (keep tabs inside the modal). Tiny modals usually
 *     get away without it; add when accessibility audits require it.
 *   - Stack support (multiple modals at once) — currently each render
 *     is independent; you'd have to render multiple <AppModal>s and
 *     ensure the right one captures Escape.
 */

import { useEffect, useRef } from 'react';
import { AlertTriangle, Info } from 'lucide-react';

export interface AppModalProps {
  title: string;
  message: string;
  variant: 'danger' | 'warning' | 'info';
  /** Defaults to "Confirm" in confirm mode, "OK" in alert mode. */
  confirmLabel?: string;
  onConfirm: () => void;
  /** Presence of `onCancel` switches the modal into "Confirm" mode. */
  onCancel?: () => void;
}

// Variant → icon + colour mapping. Adding a new variant is one entry.
const VARIANTS = {
  danger: {
    Icon: AlertTriangle,
    iconColor: 'text-red-500',
    confirmButton: 'bg-red-500 text-white hover:bg-red-600',
  },
  warning: {
    Icon: AlertTriangle,
    iconColor: 'text-yellow-500',
    confirmButton: 'bg-yellow-500 text-[#0d0d0d] hover:bg-yellow-600',
  },
  info: {
    Icon: Info,
    iconColor: 'text-[#55bde3]',
    confirmButton: 'bg-white text-[#0d0d0d] hover:bg-[#e5e5e5]',
  },
} as const;

export function AppModal({
  title,
  message,
  variant,
  confirmLabel,
  onConfirm,
  onCancel,
}: AppModalProps) {
  const { Icon, iconColor, confirmButton } = VARIANTS[variant];
  const isConfirmMode = onCancel !== undefined;
  const resolvedConfirmLabel =
    confirmLabel ?? (isConfirmMode ? 'Confirm' : 'OK');

  // Auto-focus the confirm button so Enter activates it without an
  // extra Tab. Runs once on mount because deps are empty.
  const confirmRef = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    confirmRef.current?.focus();
  }, []);

  // Escape key dismisses in confirm mode. We attach to window so the
  // listener works even when focus is somewhere weird (e.g. a portal).
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && isConfirmMode) onCancel?.();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [isConfirmMode, onCancel]);

  // Backdrop click only dismisses in confirm mode. Alert mode forces
  // the user to press OK so important messages aren't fat-fingered away.
  const handleBackdropClick = () => {
    if (isConfirmMode) onCancel?.();
  };

  return (
    <div
      role="dialog"
      aria-modal="true"
      aria-labelledby="app-modal-title"
      className="fixed inset-0 z-50 flex items-center justify-center p-4 bg-black/70"
      onClick={handleBackdropClick}
    >
      {/* Stop propagation so clicks on the card don't bubble to the
          backdrop and dismiss the modal. */}
      <div
        className="bg-[#0d0d0d] border border-[#252525] max-w-md w-full p-6"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex items-start gap-4 mb-4">
          <Icon
            className={`w-6 h-6 shrink-0 mt-0.5 ${iconColor}`}
            aria-hidden
          />
          <div className="flex-1 min-w-0">
            <h2
              id="app-modal-title"
              className="text-white text-base font-medium mb-1 break-words"
            >
              {title}
            </h2>
            <p className="text-[#b9b8ae] text-sm whitespace-pre-wrap break-words">
              {message}
            </p>
          </div>
        </div>
        <div className="flex justify-end gap-2 pt-2">
          {isConfirmMode && (
            <button
              type="button"
              onClick={onCancel}
              className="px-4 py-2 text-sm border border-[#252525] text-[#b9b8ae] hover:text-white hover:border-white transition-colors"
            >
              Cancel
            </button>
          )}
          <button
            ref={confirmRef}
            type="button"
            onClick={onConfirm}
            className={`px-4 py-2 text-sm transition-colors ${confirmButton}`}
          >
            {resolvedConfirmLabel}
          </button>
        </div>
      </div>
    </div>
  );
}
