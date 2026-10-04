/*
 * HmRdp - picks the SessionBackend for a launch request.
 *
 * Kept out of SessionBackend.ets so the concrete backends and the interface can
 * depend on the interface alone (no import cycle).
 */
import { PendingConnection, PROTOCOL_MOONLIGHT } from '../model/RdpModels';
import { RdpSessionBackend, SessionBackend } from './SessionBackend';
import { MoonlightSessionBackend } from './MoonlightSessionBackend';

export class SessionBackendFactory {
  static create(request: PendingConnection): SessionBackend | undefined {
    if (request.protocol === PROTOCOL_MOONLIGHT) {
      // The control layer ran serverinfo -> (pair) -> launch and put the stream
      // config on the request; the session window binds the surface.
      return new MoonlightSessionBackend(request);
    }
    return new RdpSessionBackend(request.options);
  }
}
