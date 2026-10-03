/*
 * HmRdp - single subscriber for the native event stream.
 *
 * The native side raises every session event on one process-global ThreadSafe
 * Function that is bound to the UI thread's runtime (see binding/hmrdp_napi.cpp,
 * OnSessionEvent). This hub is the one place that subscribes to it and fans an
 * event out by session handle to the owning RdpNative instance, so no other
 * module needs to touch onEvent and routing is impossible to register twice.
 *
 * A Worker consumer (an audio player, a download state machine, ...) cannot take
 * the TSFN directly - the callback only ever runs on the UI thread and each
 * Worker has its own napi_env - so it subscribes here and receives events
 * forwarded by postMessage on the UI thread. See doc_agent/architecture.md
 * "ArkTS 侧线程模型".
 */
import { onEvent, RdpEventCallback, RdpEventPayload } from 'libhmrdp.so';
import { RdpEvent } from '../model/RdpModels';

/** Handles one native event for a single session handle. */
export type SessionEventHandler = (event: RdpEvent, payload: RdpEventPayload) => void;

class SessionEventHubImpl {
  private handlers: Map<number, SessionEventHandler> = new Map<number, SessionEventHandler>();
  private subscribed: boolean = false;

  /** Registers the handler that owns this session handle. */
  register(handle: number, handler: SessionEventHandler): void {
    if (!this.subscribed) {
      this.subscribed = true;
      const callback: RdpEventCallback = (handle: number, event: number,
        payload: RdpEventPayload): void => {
        const target: SessionEventHandler | undefined = this.handlers.get(handle);
        if (target !== undefined) {
          target(event as RdpEvent, payload);
        }
      };
      onEvent(callback);
    }
    this.handlers.set(handle, handler);
  }

  /** Stops routing events for a handle (the session is being torn down). */
  unregister(handle: number): void {
    this.handlers.delete(handle);
  }
}

export const SessionEventHub: SessionEventHubImpl = new SessionEventHubImpl();
