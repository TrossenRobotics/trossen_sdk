/**
 * Promise-based confirmation dialog hook.
 *
 * Wraps `AppModal` (the themed alert/confirm component) so a destructive
 * action can be guarded with a single `await`:
 *
 *   const { confirm, modalElement } = useConfirm();
 *   ...
 *   if (await confirm({ title, message, confirmLabel: 'Delete' })) doIt();
 *   ...
 *   return (<>{modalElement}{ ...page... }</>);
 *
 * This replaces the copy-pasted `appModal` useState + `showConfirm` block
 * duplicated across the dataset/config pages, and is the imperative API the
 * AppModal header comment anticipated.
 */

import { useCallback, useState } from 'react';
import { AppModal } from '@/app/components/AppModal';

export interface ConfirmOptions {
  title: string;
  message: string;
  /** Confirm button label. Defaults to "Confirm". */
  confirmLabel?: string;
  /** Visual variant; destructive actions should use 'danger' (default). */
  variant?: 'danger' | 'warning' | 'info';
}

interface PendingConfirm extends ConfirmOptions {
  resolve: (confirmed: boolean) => void;
}

export function useConfirm(): {
  confirm: (opts: ConfirmOptions) => Promise<boolean>;
  modalElement: React.ReactElement | null;
} {
  const [pending, setPending] = useState<PendingConfirm | null>(null);

  const confirm = useCallback(
    (opts: ConfirmOptions) =>
      new Promise<boolean>((resolve) => setPending({ ...opts, resolve })),
    [],
  );

  // Resolve the outstanding promise and tear down the modal. Resolving a
  // settled promise is a no-op, so React StrictMode's double-invoked
  // updater is harmless here.
  const close = useCallback((confirmed: boolean) => {
    setPending((prev) => {
      prev?.resolve(confirmed);
      return null;
    });
  }, []);

  const modalElement = pending ? (
    <AppModal
      title={pending.title}
      message={pending.message}
      variant={pending.variant ?? 'danger'}
      confirmLabel={pending.confirmLabel}
      onConfirm={() => close(true)}
      onCancel={() => close(false)}
    />
  ) : null;

  return { confirm, modalElement };
}
