"""Independent Phase05 fixed-record decoder, CSV checks and timestamp association."""
import csv
import io
import struct
import uuid
import zlib

from phase03_protocol import decode_packet

HEADER = struct.Struct('<8sHHIIIQQ16sII')
RECORD = struct.Struct('<IIQQQQIIIHBB')
RECORD_BYTES = 288
BASE_COLUMNS = ['frame_seq','sensor_frame_no','host_rx_us','rx_end_us',
                'optical_earliest_us','optical_latest_us','sensor_tick_raw',
                't0_integration_raw','t1_integration_raw','clock_ratio_uq15','frame_status_raw']
ZONE_COLUMNS = [f'{key}_{i:02d}' for i in range(64)
                for key in ('raw_qmm','snr','distance_qmm','valid')]


def optical_interval(host, end, tick, t0, t1, ratio):
    delta = (tick-t0) & 0xffffffff
    if not 16384 <= ratio <= 49152 or delta > 125000 or ((t1-t0) & 0xffffffff) > delta or end < host:
        raise ValueError('invalid depth clock/order')
    age = (delta*8*ratio+16384) >> 15
    earliest = host-age-age//200-1000
    if earliest < 0:
        raise ValueError('negative optical time')
    return earliest, end


def decode_depth(data):
    if len(data) < HEADER.size:
        raise ValueError('truncated depth header')
    magic,version,header_size,record_size,count,iterations,start,end,uid,reserved,checksum = HEADER.unpack_from(data)
    if magic != b'D5TOF1\0\0' or (version,header_size,record_size) != (1,64,288) or reserved:
        raise ValueError('unsupported depth format')
    if checksum != zlib.crc32(data[:60]):
        raise ValueError('depth header CRC')
    if not 0 < count <= 384 or end-start != 30000000 or len(data) != 64+count*288:
        raise ValueError('depth count/window/length')
    result = {'burst_id':str(uuid.UUID(bytes=uid)), 'iterations':iterations,
              'start_us':start, 'end_us':end, 'frames':[]}
    for i in range(count):
        record = data[64+i*288:64+(i+1)*288]
        if zlib.crc32(record[:284]) != struct.unpack_from('<I',record,284)[0]:
            raise ValueError('depth record CRC')
        values = RECORD.unpack_from(record)
        seq,number,host,rx_end,earliest,latest,tick,t0,t1,ratio,status,flags = values
        if seq != i+1 or flags or any(record[281:284]):
            raise ValueError('depth sequence/flags')
        native = decode_packet(record[56:281])
        if native['kind'] != 0x10 or native['focal_plane'] != 0:
            raise ValueError('expected 8x8A depth result')
        if any(native[key] != value for key,value in
               (('frame_number',number),('sensor_tick',tick),('t0',t0),('t1',t1),('status',status))):
            raise ValueError('native depth association')
        if (earliest,latest) != optical_interval(host,rx_end,tick,t0,t1,ratio):
            raise ValueError('optical interval differs from raw clock evidence')
        if not start <= earliest <= latest <= end:
            raise ValueError('depth outside capture window')
        if result['frames']:
            previous = result['frames'][-1]
            if ((number-previous['sensor_frame_no']) & 0xffffffff) != 1 or host <= previous['host_rx_us']:
                raise ValueError('depth frame gap/order')
        frame = dict(zip(BASE_COLUMNS,[seq,number,host,rx_end,earliest,latest,tick,t0,t1,ratio,status]))
        frame['native'] = native
        result['frames'].append(frame)
    return result


def validate_depth_csv(data, decoded):
    lines = data.decode('utf-8').splitlines()
    metadata = dict(line[2:].split('=',1) for line in lines if line.startswith('# ') and '=' in line)
    if metadata.get('phase05_depth_csv') != '1' or metadata.get('burst_id') != decoded['burst_id']:
        raise ValueError('depth CSV schema/burst')
    reader = csv.DictReader(line for line in lines if not line.startswith('#'))
    if reader.fieldnames != BASE_COLUMNS+ZONE_COLUMNS:
        raise ValueError('depth CSV columns')
    rows = list(reader)
    if len(rows) != len(decoded['frames']):
        raise ValueError('depth CSV count')
    for row,frame in zip(rows,decoded['frames']):
        if any(int(row[key]) != frame[key] for key in BASE_COLUMNS):
            raise ValueError('depth CSV timestamp/identity')
        for i,zone in enumerate(frame['native']['zones']):
            raw,snr = zone['distance_raw_qmm'],zone['snr_raw']
            valid = bool(frame['frame_status_raw'] & 1 and not frame['frame_status_raw'] & 0xc0 and raw and snr)
            corrected = min(65535,(raw*frame['clock_ratio_uq15']+16384) >> 15)
            if (int(row[f'raw_qmm_{i:02d}']) != raw or int(row[f'snr_{i:02d}']) != snr or
                int(row[f'valid_{i:02d}']) != int(valid) or
                row[f'distance_qmm_{i:02d}'] != (str(corrected) if valid else '')):
                raise ValueError('depth CSV zone differs from native result')
    return rows


def validate_rgb_index(data, start, end):
    rows = list(csv.DictReader(io.StringIO(data.decode('utf-8'))))
    previous = None
    for i,row in enumerate(rows):
        seq,dma,done,period,earliest,latest,due,size = [int(row[k]) for k in
            ('frame_seq','camera_dma_us','camera_done_us','camera_period_bound_us',
             'optical_earliest_us','optical_latest_us','scheduled_us','jpeg_bytes')]
        if seq != i+1 or due != start+i*1000000 or not due <= dma <= due+300000 or size < 4:
            raise ValueError('RGB sequence/cadence')
        if (earliest,latest) != (dma-period,done) or not start <= earliest <= dma <= done <= end or period <= 0:
            raise ValueError('RGB optical window')
        if previous is not None and not 800000 <= dma-previous <= 1200000:
            raise ValueError('RGB frame reused or cadence missed')
        previous = dma
    return rows


def associate(rgb_rows, depth):
    """Nearest interval-midpoint association; reports uncertainty, never claims shutters sync."""
    pairs = []
    for rgb in rgb_rows:
        rt = (int(rgb['optical_earliest_us'])+int(rgb['optical_latest_us']))//2
        nearest = min(depth['frames'],key=lambda f: abs((f['optical_earliest_us']+f['optical_latest_us'])//2-rt))
        dt = (nearest['optical_earliest_us']+nearest['optical_latest_us'])//2
        pairs.append({'rgb_seq':int(rgb['frame_seq']),'depth_seq':nearest['frame_seq'],
                      'rgb_midpoint_us':rt,'depth_midpoint_us':dt,'delta_us':dt-rt,
                      'rgb_interval_us':int(rgb['optical_latest_us'])-int(rgb['optical_earliest_us']),
                      'depth_interval_us':nearest['optical_latest_us']-nearest['optical_earliest_us']})
    return pairs
