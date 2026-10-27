"""Verify independent 10 fps depth / 1 fps QXGA burst files and timer wake."""
import argparse
import csv
import io
import json
from pathlib import Path
import statistics
import time
import zlib

from phase04_console import BurstConsole,open_serial,serial,fields
from phase04_verify import wait_for_burst,state,get_json
from phase05_protocol import decode_depth,validate_depth_csv,validate_rgb_index,associate
from PIL import Image

ROOT=Path(__file__).resolve().parents[1]

def checked_file(console,burst,kind,summary,stem):
    data=console.get(f'{burst} {kind}')
    if len(data)!=summary[f'{stem}_bytes'] or zlib.crc32(data)!=int(summary[f'{stem}_crc32'],16):
        raise ValueError(f'{kind} differs from original acquisition/commit CRC')
    return data

def read_burst(console,stopped,output):
    burst=int(stopped['burst'])
    meta,summary=get_json(console,burst,'META'),get_json(console,burst,'SUMMARY')
    if not summary['complete'] or not summary['success']:
        raise ValueError('burst unsuccessful')
    for key in ('depth_gaps','depth_overflows','rgb_missed','rgb_errors','depth_errors'):
        if summary[key]: raise ValueError(f'nonzero {key}')
    if not summary['sensor_standby'] or not summary['camera_off'] or summary['led']:
        raise ValueError('sensor/LED shutdown failed')
    if (meta['depth_fps'],meta['rgb_fps'],meta['depth']['period_ms'],meta['depth']['kilo_iterations'],meta['depth']['preset'])!=(10,1,100,1000,65):
        raise ValueError('requested profile changed')
    if meta['depth']['histograms'] or (meta['rgb']['width'],meta['rgb']['height'])!=(2048,1536) or meta['rgb']['jpeg_quality']>6:
        raise ValueError('wrong sensor output mode/quality')
    if not meta['rgb']['exposure_manual'] or not 0<meta['rgb']['exposure_lines']<meta['rgb']['vts_lines']:
        raise ValueError('RGB exposure unbounded')
    start,end=summary['capture_start_us'],summary['capture_window_end_us']
    if end-start!=30000000 or not end<=summary['capture_stop_us']<=end+2000000:
        raise ValueError('capture window/duration')
    index=checked_file(console,burst,'INDEX',summary,'rgb_index')
    binary=checked_file(console,burst,'RAW',summary,'depth_binary')
    csv_data=checked_file(console,burst,'DEPTH',summary,'depth_csv')
    depth=decode_depth(binary)
    if (depth['burst_id'],depth['iterations'],depth['start_us'],depth['end_us'])!=(meta['burst_id'],1000,start,end):
        raise ValueError('binary/metadata identity mismatch')
    depth_rows=validate_depth_csv(csv_data,depth)
    rgb_rows=validate_rgb_index(index,start,end)
    if len(depth_rows)!=summary['depth_frames'] or len(rgb_rows)!=summary['rgb_frames']:
        raise ValueError('frame count mismatch')
    if not 285<=len(depth_rows)<=302 or not 28<=len(rgb_rows)<=30:
        raise ValueError('too few frames for requested cadence')
    times=[f['optical_earliest_us'] for f in depth['frames']]
    mean=statistics.mean(b-a for a,b in zip(times,times[1:]))
    if not 95000<=mean<=105000: raise ValueError('depth cadence outside 10 fps +/-5%')
    output.mkdir(parents=True,exist_ok=True)
    for name,data in (('rgb.csv',index),('depth.bin',binary),('depth.csv',csv_data)):
        (output/name).write_bytes(data)
    (output/'burst.json').write_text(json.dumps(meta,indent=2)+'\n')
    (output/'summary-device.json').write_text(json.dumps(summary,indent=2)+'\n')
    associations=associate(rgb_rows,depth)
    with (output/'associations.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=associations[0].keys()); writer.writeheader(); writer.writerows(associations)
    return {'burst':burst,'metadata':meta,'firmware_summary':summary,'verified_depth_frames':len(depth_rows),
            'rgb_frames':len(rgb_rows),'mean_depth_interval_us':mean,'achieved_depth_fps':1000000/mean,
            'max_nearest_midpoint_delta_us':max(abs(p['delta_us']) for p in associations)},rgb_rows

def reconnect(console):
    console.serial.close(); time.sleep(3)
    deadline=time.monotonic()+50
    while time.monotonic()<deadline:
        try:
            console.serial=open_serial(console.port); console.send('HOLD'); console.until('HOLD enabled=1',timeout=15)
            return
        except (serial.SerialException,OSError,TimeoutError):
            try: console.serial.close()
            except Exception: pass
            time.sleep(0.5)
    raise TimeoutError('USB reconnect after timer sleep')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',default='COM12')
    parser.add_argument('--output',type=Path,default=ROOT/'Phase05/validation/local')
    parser.add_argument('--restart',action='store_true')
    parser.add_argument('--manifest-only',action='store_true',help='skip JPEG downloads for bring-up')
    parser.add_argument('--skip-sleep-check',action='store_true')
    args=parser.parse_args(); args.output.mkdir(parents=True,exist_ok=True)
    report={'passed':False,'verified_jpegs':0}
    log=(args.output/'serial.log').open('w',encoding='utf-8')
    console=BurstConsole(args.port,log)
    try:
        if args.restart:
            console.serial.reset_input_buffer()
            console.send('REBOOT'); console.until('REBOOTING',timeout=40)
            console.until('PHASE05 boot=',timeout=40)
        console.send('HOLD'); console.until('HOLD enabled=1',timeout=30)
        stopped,active_led=wait_for_burst(console,timeout=120)
        initial,rows=read_burst(console,stopped,args.output)
        report.update(initial); report['active_led_observed']=active_led
        first=None
        for i,row in enumerate([] if args.manifest_only else rows):
            jpeg=console.get(f"{initial['burst']} JPG {row['frame_seq']}")
            if len(jpeg)!=int(row['jpeg_bytes']) or zlib.crc32(jpeg)!=int(row['jpeg_crc32'],16):
                raise ValueError('JPEG differs from acquisition CRC')
            with Image.open(io.BytesIO(jpeg)) as image:
                image.load()
                if image.size!=(2048,1536) or image.mode!='RGB': raise ValueError('JPEG decode/dimensions')
            (args.output/f"{int(row['frame_seq']):06d}.jpg").write_bytes(jpeg)
            if first is None: first=jpeg
            report['verified_jpegs']+=1
            if (i+1)%10==0: print(f'Verified {i+1}/{len(rows)} QXGA JPEGs and all depth frames',flush=True)
        if not args.skip_sleep_check:
            sleep_start=time.monotonic(); console.send('SLEEP 10'); console.until('SLEEP seconds=10',timeout=20)
            reconnect(console); wake=state(console)
            if wake['wake_reason']!='4' or int(wake['boot_count'])!=initial['metadata']['boot_count']+1:
                raise ValueError('wrong wake cause/count')
            report['sleep_to_usb_response_seconds']=time.monotonic()-sleep_start
            report['wake_status']=wake
            stopped,_=wait_for_burst(console,timeout=120)
            after,_=read_burst(console,stopped,args.output/'after-wake')
            if after['burst']==initial['burst'] or after['metadata']['burst_id']==initial['metadata']['burst_id']:
                raise ValueError('burst ID/folder reused')
            if first is not None and console.get(f"{initial['burst']} JPG 1")!=first:
                raise ValueError('earlier JPEG changed after wake')
            report['after_wake']=after
        console.send('ALLOW_SLEEP'); console.until('HOLD enabled=0')
        report['passed']=True
    except Exception as error:
        report['error']=str(error)
    finally:
        console.serial.close(); log.close()
        (args.output/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    return 0 if report['passed'] else 1

if __name__=='__main__': raise SystemExit(main())
