from pathlib import Path
import csv
import io
import struct
import sys
import unittest
import uuid
import zlib

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from phase05_protocol import (HEADER,RECORD,BASE_COLUMNS,ZONE_COLUMNS,decode_depth,
                              optical_interval,validate_depth_csv,validate_rgb_index,associate)

def fixture():
    packet=bytearray(225)
    struct.pack_into('<I',packet,1,5000)
    packet[5:9]=bytes.fromhex('1001d800')
    struct.pack_into('<I',packet,9,77)
    for i in range(64): struct.pack_into('<HB',packet,21+i*3,1000+i,30)
    struct.pack_into('<IIBBH',packet,213,2000,3500,1,0,0xe0f7)
    earliest,latest=optical_interval(1040000,1040500,5000,2000,3500,32768)
    record=bytearray(288)
    RECORD.pack_into(record,0,1,77,1040000,1040500,earliest,latest,5000,2000,3500,32768,1,0)
    record[56:281]=packet
    struct.pack_into('<I',record,284,zlib.crc32(record[:284]))
    header=bytearray(HEADER.pack(b'D5TOF1\0\0',1,64,288,1,1000,900000,30900000,
                               uuid.UUID('12345678-1234-4234-8234-123456789abc').bytes,0,0))
    struct.pack_into('<I',header,60,zlib.crc32(header[:60]))
    return header+record

def csv_fixture(decoded):
    stream=io.StringIO()
    stream.write('# phase05_depth_csv=1\n# burst_id='+decoded['burst_id']+'\n')
    writer=csv.DictWriter(stream,fieldnames=BASE_COLUMNS+ZONE_COLUMNS,lineterminator='\n')
    writer.writeheader()
    for f in decoded['frames']:
        row={k:f[k] for k in BASE_COLUMNS}
        for i,z in enumerate(f['native']['zones']):
            row.update({f'raw_qmm_{i:02d}':z['distance_raw_qmm'],f'snr_{i:02d}':z['snr_raw'],
                        f'distance_qmm_{i:02d}':z['distance_raw_qmm'],f'valid_{i:02d}':1})
        writer.writerow(row)
    return stream.getvalue().encode()

class Phase05ProtocolTests(unittest.TestCase):
    def test_fixed_layout_and_all_64_csv_zones(self):
        data=fixture(); self.assertEqual((HEADER.size,RECORD.size,len(data)),(64,56,352))
        self.assertEqual(data[:24].hex(),'4435544f46310000010040002001000001000000e8030000')
        decoded=decode_depth(data)
        self.assertEqual(decoded['iterations'],1000)
        self.assertEqual(decoded['frames'][0]['optical_earliest_us'],1014880)
        self.assertEqual(len(validate_depth_csv(csv_fixture(decoded),decoded)),1)
        self.assertEqual(decoded['frames'][0]['native']['zones'][-1]['distance_raw_qmm'],1063)

    def test_truncation_unknown_version_and_crc_damage(self):
        data=fixture()
        for damaged in (data[:20],data[:-1],data[:8]+b'\x02'+data[9:],data[:100]+bytes([data[100]^1])+data[101:]):
            with self.assertRaises(ValueError): decode_depth(damaged)

    def test_valid_crc_does_not_hide_wrong_native_association(self):
        data=fixture(); struct.pack_into('<I',data,64+4,78)
        struct.pack_into('<I',data,64+284,zlib.crc32(data[64:64+284]))
        with self.assertRaisesRegex(ValueError,'association'): decode_depth(data)

    def test_clock_order_wrap_and_out_of_window(self):
        self.assertEqual(optical_interval(100000,100500,1000,0xfffffff0,800,32768),(90832,100500))
        with self.assertRaises(ValueError): optical_interval(100000,100500,2000,3000,4000,32768)
        data=fixture(); struct.pack_into('<Q',data,64+24,1)
        struct.pack_into('<I',data,64+284,zlib.crc32(data[64:64+284]))
        with self.assertRaisesRegex(ValueError,'optical interval'): decode_depth(data)

    def test_csv_rejects_changed_timestamp_and_depth(self):
        decoded=decode_depth(fixture()); data=csv_fixture(decoded)
        for damaged in (data.replace(b'1040000',b'1040001'),data.replace(b'1000,30,1000,1',b'1000,30,1001,1')):
            with self.assertRaises(ValueError): validate_depth_csv(damaged,decoded)

    def test_rgb_and_nearest_timestamp_association(self):
        data=(b'frame_seq,camera_dma_us,camera_done_us,camera_period_bound_us,optical_earliest_us,optical_latest_us,scheduled_us,jpeg_bytes,jpeg_crc32\n'
              b'1,1100000,1190000,100000,1000000,1190000,900000,1000,12345678\n')
        rgb=validate_rgb_index(data,900000,30900000)
        association=associate(rgb,decode_depth(fixture()))[0]
        self.assertEqual(association['depth_seq'],1)
        self.assertEqual(association['rgb_interval_us'],190000)
        self.assertEqual(association['delta_us'],-67310)
        with self.assertRaises(ValueError): validate_rgb_index(data.replace(b'900000,1000',b'800000,1000'),900000,30900000)

if __name__=='__main__': unittest.main()
