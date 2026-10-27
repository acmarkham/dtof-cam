"""Associate a copied Phase05 burst's RGB frames with nearest timestamped depth maps."""
import argparse
import csv
import json
from pathlib import Path
import zlib

from phase05_protocol import decode_depth,validate_depth_csv,validate_rgb_index,associate

def load_burst(path):
    meta=json.loads((path/'burst.json').read_text())
    summary=json.loads((path/'summary.json').read_text())
    if meta['firmware']!='Phase05' or not summary['complete'] or not summary['success']:
        raise ValueError('requires a complete successful Phase05 burst')
    contents={}
    for name,stem in (('depth.bin','depth_binary'),('depth.csv','depth_csv'),('rgb.csv','rgb_index')):
        data=(path/name).read_bytes()
        if len(data)!=summary[f'{stem}_bytes'] or zlib.crc32(data)!=int(summary[f'{stem}_crc32'],16):
            raise ValueError(f'{name}: original commit CRC/length mismatch')
        contents[name]=data
    depth=decode_depth(contents['depth.bin'])
    if (depth['burst_id'],depth['start_us'],depth['end_us'],depth['iterations'])!=(
        meta['burst_id'],summary['capture_start_us'],summary['capture_window_end_us'],meta['depth']['kilo_iterations']):
        raise ValueError('burst identity/configuration differs')
    validate_depth_csv(contents['depth.csv'],depth)
    rgb=validate_rgb_index(contents['rgb.csv'],depth['start_us'],depth['end_us'])
    if len(rgb)!=summary['rgb_frames'] or len(depth['frames'])!=summary['depth_frames']:
        raise ValueError('frame counts differ from summary')
    for row in rgb:
        data=(path/f"{int(row['frame_seq']):06d}.jpg").read_bytes()
        if len(data)!=int(row['jpeg_bytes']) or zlib.crc32(data)!=int(row['jpeg_crc32'],16):
            raise ValueError('JPEG differs from original acquisition')
    return meta,rgb,depth

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('burst',type=Path)
    parser.add_argument('--output',type=Path,required=True,help='new CSV file; existing output is preserved')
    args=parser.parse_args()
    meta,rgb,depth=load_burst(args.burst)
    pairs=[dict(burst_id=meta['burst_id'],rgb_file=f"{p['rgb_seq']:06d}.jpg",**p) for p in associate(rgb,depth)]
    with args.output.open('x',newline='',encoding='utf-8') as f:
        writer=csv.DictWriter(f,fieldnames=pairs[0].keys()); writer.writeheader(); writer.writerows(pairs)
    print(f"Associated {len(rgb)} RGB frames with {len(depth['frames'])} depth frames: {args.output}")

if __name__=='__main__': main()
