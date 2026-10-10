import { PendingConnection, PROTOCOL_MOONLIGHT } from '../model/Connections';
import { RdpSessionBackend } from './RdpSessionBackend';
import { SessionBackend } from './SessionBackend';
import { MoonlightSessionBackend } from './MoonlightSessionBackend';

// Kept out of SessionBackend.ets so the backends and the interface avoid an import cycle.
export class SessionBackendFactory {
  static create(request: PendingConnection): SessionBackend | undefined {
    if (request.protocol === PROTOCOL_MOONLIGHT) {
      return new MoonlightSessionBackend(request);
    }
    return new RdpSessionBackend(request.options);
  }
}
