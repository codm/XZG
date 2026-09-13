"""Bound idle preconnections in the pinned stock HTTP server (UI and API)."""
from pathlib import Path

MARKER = '// CZC: idle HTTP sockets must not block ready UI/API requests.'

def patch(sdk):
    directory = Path(sdk) / 'libraries/WebServer/src'
    header = directory / 'WebServer.h'
    source = directory / 'WebServer.cpp'
    h, s = header.read_text(), source.read_text()
    if MARKER in h and MARKER in s:
        return
    assert MARKER not in h and MARKER not in s, 'Incomplete WebServer patch'
    field = '  WiFiClient  _currentClient;'
    accept = '    _currentClient = _server.available();'
    close = 'void WebServer::close() {\n  _server.close();'
    assert h.count(field)==1 and s.count(accept)==1 and s.count(close)==1, 'Unexpected WebServer SDK source'
    h = h.replace(field, field + '\n  ' + MARKER + '''
  struct IdleHttpClient { WiFiClient client; unsigned long since=0; } _idleHttp[3];''')
    s = s.replace(accept, '    ' + MARKER + '''
    for(auto &p: _idleHttp) {
      if(!p.client.connected()) { p.client=WiFiClient(); continue; }
      if(!_currentClient && p.client.available()) {
        _currentClient=p.client; p.client=WiFiClient();
      } else if(millis()-p.since>HTTP_MAX_DATA_WAIT) p.client.stop();
    }
    if(!_currentClient) {
      WiFiClient next=_server.available();
      if(next && next.available()) _currentClient=next;
      else if(next) {
        for(auto &p: _idleHttp) if(!p.client.connected()) {
          p.client=next; p.since=millis(); break;
        }
      }
    }''')
    s = s.replace(close, close + '\n  for(auto &p: _idleHttp) p.client.stop();')
    header.write_text(h)
    source.write_text(s)

if __name__ == '__main__':
    import sys
    patch(sys.argv[1])
