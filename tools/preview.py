#!/usr/bin/env python3
"""Browser-only fixture; never connects to a tuner or claims hardware evidence."""
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
import json, time, argparse, urllib.parse
p=argparse.ArgumentParser();p.add_argument('--port',type=int,default=8776);a=p.parse_args()
root=Path(__file__).resolve().parents[1]/'build-sunton/generated/public'
started=time.monotonic();operations={};fault=False
state={'boot_id':'1'*32,'source_token':'2'*32,'connected':True,'busy':False,'source':'local','fwd_power':42.5,'ref_power':.8,'swr':1.32,'band':'20m','mode':4,'antenna':1,'meter_age_ms':0,'radio':{'state':'ready','tx_frequency_hz':14074000,'band':'20m','rx_tx':'rx'},'external_antenna':{'confirmed':2,'result':'success','evidence':2}}
config={'revision':1,'enabled':False,'host':'','port':1883,'receiver':0,'watts':1000,'psu_voltage':13.8,'brightness':80,'minimum':15,'maximum':100,'dark_adc':3500,'bright_adc':300,'rotation':0,'view':0,'swr_threshold':2,'username':'admin','uart_rx':35,'uart_tx':22,'bands':[{'ldg':0,'external':0} for _ in range(11)]}
class Handler(SimpleHTTPRequestHandler):
 def __init__(self,*args,**kw):super().__init__(*args,directory=str(root),**kw)
 def log_message(self,*args):pass
 def reply(self,value,status=200):
  data=json.dumps(value).encode();self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
 def do_GET(self):
  path=urllib.parse.urlparse(self.path).path
  if path=='/api/v2/state':return self.reply(dict(state,sequence=int((time.monotonic()-started)*100),uptime_ms=int((time.monotonic()-started)*1000)))
  if path=='/api/v2/capabilities':return self.reply({'api_version':2,'board':'sunton','firmware':'0.1.0','release_repository':''})
  if path.startswith('/api/v2/config/'):return self.reply(config)
  if path=='/api/v2/ws-token':return self.reply({'error':'fixture_polling_only'},503)
  if path.startswith('/api/v2/commands/'):
   item=operations.get(path.rsplit('/',1)[1]);return self.reply(item or {'error':'unknown_operation'},200 if item else 404)
  if path=='/api/v2/diagnostics':return self.reply({'free_internal':48000,'largest_internal':32000,'mqtt_connected':False})
  if path.startswith('/api/'):return self.reply({'error':'fixture_unavailable'},404)
  return super().do_GET()
 def do_POST(self):
  raw=self.rfile.read(int(self.headers.get('Content-Length','0')))
  try:data=json.loads(raw)
  except:return self.reply({'error':'invalid_json'},400)
  if self.path.startswith('/api/v2/config/'):return self.reply({'error':'invalid_request'},400)
  if self.path=='/api/v2/commands':
   response={'request_id':data['request_id'],'state':'done','result':'success','command':data['command']};operations[data['request_id']]=response;return self.reply(response,202)
  return self.reply({'accepted':True})
ThreadingHTTPServer(('127.0.0.1',a.port),Handler).serve_forever()
