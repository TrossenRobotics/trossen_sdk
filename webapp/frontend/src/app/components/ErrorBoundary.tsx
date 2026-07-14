/**
 * Generic React error boundary.
 *
 * Primary use: contain the embedded Rerun WASM viewer, whose teardown can throw
 * "attempted to take ownership of Rust value while it was borrowed" from
 * `WebViewer.componentWillUnmount` when the recorder's gRPC server dies (e.g. a
 * mid-session crash). Without a boundary that throw bubbles to react-router's
 * default error element and replaces the whole app with the raw
 * "Unexpected Application Error!" page. Wrapping the viewer keeps a render-time
 * failure local to the viewer pane; the route-level errorElement in App.tsx is
 * the backstop for anything a child boundary can't catch (e.g. a throw during
 * the same commit that unmounts this boundary).
 */
import { Component } from 'react';
import type { ErrorInfo, ReactNode } from 'react';
import { logError } from '@/lib/logger';

interface Props {
  children: ReactNode;
  /** Rendered in place of the children once an error is caught. */
  fallback?: ReactNode;
  /** Label included in the logged message, to tell boundaries apart. */
  label?: string;
  /**
   * When this value changes, the boundary clears a previously-caught error and
   * retries rendering its children. Used to recover after an intentional
   * remount (e.g. reconnecting the Rerun viewer on a resume): the old child's
   * teardown can throw, which would otherwise leave the fallback latched even
   * though a fresh child is ready to mount.
   */
  resetKey?: unknown;
}

interface State {
  hasError: boolean;
}

export class ErrorBoundary extends Component<Props, State> {
  state: State = { hasError: false };

  static getDerivedStateFromError(): State {
    return { hasError: true };
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    logError(`ErrorBoundary${this.props.label ? ` [${this.props.label}]` : ''} caught: ${error.message}`, {
      component: 'ErrorBoundary',
      stack: info.componentStack ?? undefined,
    });
  }

  componentDidUpdate(prev: Props): void {
    if (this.state.hasError && prev.resetKey !== this.props.resetKey) {
      this.setState({ hasError: false });
    }
  }

  render(): ReactNode {
    if (this.state.hasError) return this.props.fallback ?? null;
    return this.props.children;
  }
}
