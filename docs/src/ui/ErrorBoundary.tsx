import { Component, type ErrorInfo, type ReactNode } from 'react';

/* Without this, one throw anywhere in the tree unmounts the whole app and
 * leaves a white page with nothing to go on. The realistic cause is version
 * skew: firmware and this page cannot be updated atomically, so a board can
 * always be running a schema this build does not expect. Skew should degrade
 * into a message, not a blank screen. */

interface Props {
  children: ReactNode;
}

interface State {
  error: Error | null;
}

export class ErrorBoundary extends Component<Props, State> {
  state: State = { error: null };

  static getDerivedStateFromError(error: Error): State {
    return { error };
  }

  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error('Unhandled render error:', error, info.componentStack);
  }

  render() {
    const { error } = this.state;
    if (!error) return this.props.children;

    return (
      <div className="crash">
        <h2>The page hit an error</h2>
        <p>
          Most often this means the board is running firmware that does not
          match this page. Reloading recovers the editor; your room config is
          saved locally and on the device.
        </p>
        <pre>{error.message}</pre>
        <div className="crash-actions">
          <button onClick={() => this.setState({ error: null })}>
            Try to continue
          </button>
          <button onClick={() => window.location.reload()}>Reload</button>
        </div>
      </div>
    );
  }
}
