#!/usr/bin/env python3
"""Read-only acceptance against a real v2 adapter; no valid hardware commands.
Credentials come from LDG_USER/LDG_PASSWORD. Optional --soak-seconds writes JSONL
heap/reset evidence. This script is not a simulator and requires a real target.
"""
import argparse,base64,http.client,json,os,socket,time,urllib.parse,urllib.request,gzip,threading,re
p=argparse.ArgumentParser();p.add_argument('url');p.add_argument('--soak-seconds',type=int,default=0);p.add_argument('--output',default='network-evidence.jsonl');a=p.parse_args()
u=urllib.parse.urlparse(a.url);host=u.hostname;port=u.port or 80
if u.scheme!='http' or not host:raise SystemExit('Use the device http:// address')
user=os.environ.get('LDG_USER','admin');password=os.environ.get('LDG_PASSWORD')
if password is None:raise SystemExit('Set LDG_PASSWORD; credentials are not written into evidence')
auth='Basic '+base64.b64encode(f'{user}:{password}'.encode()).decode()
def request(path):
    connection=http.client.HTTPConnection(host,port,timeout=8);connection.request('GET',path,headers={'Authorization':auth,'Accept-Encoding':'gzip'});r=connection.getresponse();data=r.read();encoding=r.getheader('Content-Encoding');code=r.status;connection.close()
    if code!=200:raise RuntimeError(f'GET {path}: {code}')
    return gzip.decompress(data) if encoding=='gzip' else data
html=request('/').decode();assets=re.findall(r'(?:src|href)="(/assets/[^"]+)"',html)
# An invalid command can prove header/body reassembly without driving UART.
body=json.dumps({'command':'no_such_command','request_id':'12345678-1234-4000-8000-123456789abc','source_token':'0'*32,'expires_uptime_ms':1}).encode()
header=(f'POST /api/v2/commands HTTP/1.1\r\nHost: {host}\r\nAuthorization: {auth}\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n').encode()
with socket.create_connection((host,port),timeout=8) as s:
    for piece in (header[:3],header[3:27],header[27:],body[:1],body[1:7],body[7:]):s.sendall(piece);time.sleep(.05)
    response=http.client.HTTPResponse(s);response.begin();answer=json.loads(response.read())
    assert response.status==400 and answer.get('error')=='invalid_request',answer
# Slow resource consumer coexists with short API reads.
failures=[]
def slow_reader(path):
    try:
        with socket.create_connection((host,port),timeout=8) as s:
            s.sendall(f'GET {path} HTTP/1.1\r\nHost: {host}\r\nAuthorization: {auth}\r\nConnection: close\r\n\r\n'.encode())
            response=http.client.HTTPResponse(s);response.begin();assert response.status==200
            data=bytearray()
            while True:
                part=response.read(128)
                if not part:break
                data.extend(part);time.sleep(.02)
            if response.getheader('Content-Encoding')=='gzip':gzip.decompress(data)
    except Exception as e:failures.append(str(e))
thread=threading.Thread(target=slow_reader,args=(assets[-1],));thread.start();latencies=[]
for _ in range(10):start=time.monotonic();json.loads(request('/api/v2/state'));latencies.append(time.monotonic()-start)
thread.join();assert not failures,failures
with socket.create_connection((host,port),timeout=8):json.loads(request('/api/v2/state'))
started=time.monotonic();boot=None
with open(a.output,'w') as output:
    output.write(json.dumps({'fragmented_post':True,'slow_reader':True,'empty_preconnect':True,'short_request_ms':[round(v*1000,1) for v in latencies]})+'\n');output.flush()
    while True:
        state=json.loads(request('/api/v2/state'));diag=json.loads(request('/api/v2/diagnostics'))
        if boot is not None and boot!=state['boot_id']:raise RuntimeError('Device rebooted during acceptance')
        boot=state['boot_id'];output.write(json.dumps({'at':time.time(),'state':state,'diagnostics':diag})+'\n');output.flush()
        if time.monotonic()-started>=a.soak_seconds:break
        time.sleep(min(30,a.soak_seconds-(time.monotonic()-started)))
print('Real adapter checks completed; evidence:',a.output)
