"""Compare the full frozen BRID references with the original publisher ZIP, without inference."""
from pathlib import Path
from decimal import Decimal
from collections import Counter
from zipfile import ZipFile
import hashlib,importlib.util,io,json,re,statistics,sys,types,unittest

def verify(base):
 manifest=json.loads((base/'verification.json').read_bytes());helper=base.parent/'brid-decoder-frontend-2026-11-06/verify_evidence.py'
 assert hashlib.sha256(helper.read_bytes()).hexdigest()==manifest['archive_loader_sha256']
 spec=importlib.util.spec_from_file_location('fixed_annotation_archive_loader',helper)
 common=importlib.util.module_from_spec(spec);spec.loader.exec_module(common)
 files=common.load_bundle(base);read=lambda n:json.loads(files[n])
 expected=read('analysis/result.json');protocol=read('reference/protocol.json')
 assert common.digest(files['reference/protocol.json'])==expected['protocol_sha256']
 assert common.digest(files['sources/annotations.zip'])==expected['annotation_archive_sha256']
 assert protocol['fixed_denominator']==len(protocol['cases'])==93
 parse=common.namespace(files,'analysis/audit_annotations.py',{'parse'})['parse']
 # Run the original focused parser regressions without writing/importing an archive member.
 module=types.ModuleType('audit_annotations');module.parse=parse;previous=sys.modules.get('audit_annotations');sys.modules['audit_annotations']=module
 try:
  tests=types.ModuleType('fixed_annotation_checks');exec(compile(files['analysis/test_annotations.py'],'analysis/test_annotations.py','exec'),tests.__dict__)
  result=unittest.TextTestRunner(stream=io.StringIO()).run(unittest.defaultTestLoader.loadTestsFromModule(tests))
  assert result.wasSuccessful()and result.testsRun==4
 finally:
  if previous is None:sys.modules.pop('audit_annotations',None)
  else:sys.modules['audit_annotations']=previous
 meta=read('sources/zenodo-record.json');advertised={f['key']:f['checksum']for f in meta['files']}
 assert advertised['annotations.zip']=='md5:'+hashlib.md5(files['sources/annotations.zip']).hexdigest()
 assert advertised['BRID - Description.pdf']=='md5:'+hashlib.md5(files['sources/description.pdf']).hexdigest()
 table={m.group(1):(m.group(2),int(m.group(3)))for m in re.finditer(r'^\s*(\d{4})\s+(M[234]-[0-9]+-[A-Z]+)\s+([234])\s+',files['sources/description.txt'].decode(),re.M)};assert len(table)==93
 rows=[];first=Counter();prefix=Counter();beats=downbeats=0
 with ZipFile(io.BytesIO(files['sources/annotations.zip']))as z:
  assert len(z.namelist())==len(set(z.namelist()))==186 and z.testzip()is None
  names={n for n in z.namelist()if n.endswith('.beats')};consumed=[]
  for index,case in enumerate(protocol['cases'],1):
   assert case['id']==index;name='Annotations/beats/'+case['name']+'.beats';assert name in names
   original=z.read(name);times,positions=parse(original);info=z.getinfo(name)
   assert times==case['reference_seconds_text']and positions==case['bar_positions']
   assert len(times)==case['source']['reference_count']and sum(v==1 for v in positions)==case['source']['position1_count']
   assert all(a!=b for a,b in zip(positions,positions[1:]))
   track=case['name'].split('] ',1)[1];gid=case['name'][1:5];assert table[gid]==(track,int(track[1]))
   first[positions[0]]+=1;prefix[track[1]]+=1;beats+=len(times);downbeats+=sum(v==1 for v in positions);consumed.append(name)
   tempo=Decimal(z.read('Annotations/tempo/'+case['name']+'.bpm').decode().strip());median=statistics.median(Decimal(60)/(Decimal(b)-Decimal(a))for a,b in zip(times,times[1:]))
   rows.append(dict(id=index,name=case['name'],annotation_member=name,annotation_sha256=common.digest(original),ZIP_CRC32=f'{info.CRC:08x}',reference_events=len(times),position_one_events=sum(v==1 for v in positions),first_position=positions[0],instrument_count=table[gid][1],timestamps_identical=True,positions_identical=True,alternating_1_2=True,published_BPM=str(tempo),median_adjacent_BPM=str(median)))
  assert set(consumed)==names and len(names)==93
 assert rows==expected['cases']and beats==expected['beat_events']==4342 and downbeats==expected['position_one_downbeats']==2205
 assert dict(first)=={int(k):v for k,v in expected['first_position_counts'].items()}and dict(prefix)==expected['instrument_count_counts']
 assert expected['qualified']is False and expected['confidence']=='Unknown'and expected['no_predictions_rescored']is True
 assert meta['metadata']['license']==expected['publisher_Zenodo_license']=={'id':'cc-by-4.0'}
 return dict(verified_members=len(files),fixed_denominator=93,reference_cases_verified=93,beat_timestamps=4342,position_one_labels=2205,leading_position_two_cases=first[2],focused_parser_checks=4,all_timestamps_and_positions_exact=True,reference_convention_correction_warranted=False,predictions_rescored=False,qualified=False,confidence='Unknown',models_GPU_audio_or_network_started=False)

if __name__=='__main__':print(json.dumps(verify(Path(__file__).resolve().parent),indent=2))
