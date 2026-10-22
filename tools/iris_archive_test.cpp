#include <assert.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "../ESP32Drop/examples/SerialPhotoBridge/iris_archive.h"

uint32_t crc32(uint32_t crc, const uint8_t *data, uint32_t n) {
  crc = ~crc;
  for (uint32_t i=0;i<n;++i) {
    crc ^= data[i];
    for (int b=0;b<8;++b) crc=(crc>>1) ^ (0xedb88320u & (0u-(crc&1)));
  }
  return ~crc;
}
void add(std::vector<uint8_t>& a,const char* name,std::vector<uint8_t> data,uint32_t mode=0100644) {
  char h[111];
  snprintf(h,sizeof(h),"070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x",
           1u,mode,0u,0u,1u,0u,(unsigned)data.size(),0u,0u,0u,0u,(unsigned)strlen(name)+1,0u);
  a.insert(a.end(),h,h+110); a.insert(a.end(),name,name+strlen(name)+1);
  while(a.size()%4) a.push_back(0);
  a.insert(a.end(),data.begin(),data.end());
  while(a.size()%4) a.push_back(0);
}
void add_odc(std::vector<uint8_t>& a,const char* name,std::vector<uint8_t> data,uint32_t mode=0100644) {
  char h[77];
  snprintf(h,sizeof(h),"070707%06o%06o%06o%06o%06o%06o%06o%011o%06o%011o",
           0u,1u,mode,0u,0u,1u,0u,0u,(unsigned)strlen(name)+1,(unsigned)data.size());
  a.insert(a.end(),h,h+76); a.insert(a.end(),name,name+strlen(name)+1);
  a.insert(a.end(),data.begin(),data.end());
}
bool deliver(iris::Archive &a,const std::vector<uint8_t>& bytes,uint32_t id=1,bool bad_crc=false) {
  for(uint32_t p=0;p<bytes.size();) {
    uint32_t n=uint32_t(bytes.size())-p; if(n>13)n=13;
    if(!a.append(id,p,bytes.data()+p,n,crc32))return false;
    p+=n;
  }
  const uint8_t *jpeg; uint32_t size;
  return a.commit(id,bytes.size(),crc32(0,bytes.data(),bytes.size()) ^ uint32_t(bad_crc),jpeg,size);
}

using PreviewState = iris::ArchivePreview::State;
iris::ArchivePreview preview_bytes(const std::vector<uint8_t> &bytes, uint32_t capacity = 4096) {
  // Returned pointers are borrowed from bytes, which remains alive in the caller.
  iris::Archive a; a.data=const_cast<uint8_t *>(bytes.data());
  a.capacity=capacity; a.id=73; a.used=uint32_t(bytes.size()); a.active=true;
  const auto result=a.preview();
  assert(a.used==bytes.size() && a.id==73 && a.active && !a.error[0]);
  if (result.jpeg) {
    assert(result.state==PreviewState::Jpeg && result.transfer_id==73);
    assert(result.jpeg==bytes.data()+result.archive_offset);
    assert(result.available<=result.total);
    assert(result.archive_offset<=bytes.size() && result.available<=bytes.size()-result.archive_offset);
    assert(result.name>=reinterpret_cast<const char *>(bytes.data()));
    assert(result.name+result.name_length<reinterpret_cast<const char *>(bytes.data()+bytes.size()));
  } else assert(!result.available && !result.total && !result.complete);
  return result;
}
void preview_test() {
  const std::vector<uint8_t> photo{0xff,0xd8,0xff,0xe0,0,4,0x41,0x42,0xff,0xd9,0x55};
  // Every split point covers headers, names, pre-data padding, data and tail
  // padding. The buffer ends at the split: the preview cannot read future data.
  for (bool odc : {false,true}) {
    std::vector<uint8_t> bytes;
    auto emit=[&](const char *name,std::vector<uint8_t> data,uint32_t mode) {
      if (odc) add_odc(bytes,name,data,mode); else add(bytes,name,data,mode);
    };
    emit(".",{},0040755);
    emit("./metadata",{0,5,0x16,7,1,2},0100644); // detect AppleDouble by bytes, not name
    const uint32_t header=uint32_t(bytes.size());
    emit("./photo.jpg",photo,0100644);
    const uint32_t raw_offset=header+(odc?76u:110u)+12u;
    // strlen("./photo.jpg") is 11, plus its terminating NUL.
    const uint32_t offset=odc?raw_offset:(raw_offset+3u)&~3u;
    emit("./._photo.jpg",{0,5,0x16,7},0100644);
    emit("TRAILER!!!",{},0);
    for (size_t n=0;n<=bytes.size();++n) {
      const std::vector<uint8_t> cut(bytes.begin(),bytes.begin()+n);
      const auto p=preview_bytes(cut);
      if (n<offset+3) {
        if (p.state!=PreviewState::Waiting)
          fprintf(stderr,"preview split: odc=%d received=%zu photo_offset=%u state=%u error=%s\n",
                  int(odc),n,offset,unsigned(p.state),p.error);
        assert(p.state==PreviewState::Waiting);
      }
      else {
        assert(p.state==PreviewState::Jpeg && p.total==photo.size());
        const uint32_t arrived=uint32_t(n)-offset;
        assert(p.available==(arrived<photo.size()?arrived:photo.size()));
        assert(p.complete==(p.available==photo.size()));
        assert(p.name_length==11 && !memcmp(p.name,"./photo.jpg",11));
        assert(!memcmp(p.jpeg,photo.data(),p.available));
      }
    }
    std::vector<uint8_t> storage(4096);
    iris::Archive a; a.data=storage.data(); a.capacity=storage.size();
    for (uint32_t n=0;n<bytes.size();++n) {
      assert(a.append(111,n,bytes.data()+n,1,crc32));
      const uint32_t saved_crc=a.crc, saved_used=a.used;
      (void)a.preview(); assert(a.crc==saved_crc && a.used==saved_used && a.active);
    }
    assert(a.preview().complete);
    const uint8_t *jpeg=nullptr; uint32_t size=0;
    auto verified=a;
    assert(verified.commit(111,bytes.size(),crc32(0,bytes.data(),bytes.size()),jpeg,size));
    assert(size==photo.size() && !memcmp(jpeg,photo.data(),size));
    assert(!a.commit(111,bytes.size(),a.crc^1,jpeg,size)); // preview never substitutes for CRC
    assert(a.preview().state==PreviewState::Waiting && !a.preview().jpeg);
    assert(a.append(112,0,bytes.data(),5,crc32));
    assert(!a.preview().jpeg && a.preview().transfer_id==112);
  }
  // Missing alignment bytes cannot be mistaken for the start of JPEG data.
  for (const char *name : {"a","ab","abc","abcd"}) {
    std::vector<uint8_t> bytes; add(bytes,name,photo);
    const uint32_t offset=(110u+uint32_t(strlen(name))+1u+3u)&~3u;
    for (uint32_t n=110;n<offset+3;++n)
      assert(preview_bytes(std::vector<uint8_t>(bytes.begin(),bytes.begin()+n)).state==PreviewState::Waiting);
    assert(preview_bytes(bytes).complete);
  }
  std::vector<uint8_t> good; add(good,"a.jpg",photo);
  const auto valid=preview_bytes(good);
  assert(valid.state==PreviewState::Jpeg && valid.complete);
  std::vector<uint8_t> crc_newc=good; crc_newc[5]='2';
  assert(preview_bytes(crc_newc).state==PreviewState::Jpeg);

  std::vector<uint8_t> multi=good; add(multi,"b.jpg",photo);
  auto p=preview_bytes(multi); assert(p.state==PreviewState::Multiple && !p.jpeg);
  std::vector<uint8_t> movie=good; add(movie,"b.mov",{0,0,0,24,'f','t','y','p'});
  p=preview_bytes(movie); assert(p.state==PreviewState::Unsupported && !p.jpeg);
  std::vector<uint8_t> heic; add(heic,"a.heic",{0,0,0,24,'f','t','y','p','h','e','i','c'});
  assert(preview_bytes(heic).state==PreviewState::Unsupported);
  std::vector<uint8_t> named_sidecar; add(named_sidecar,"./._fake.jpg",photo);
  assert(preview_bytes(named_sidecar).state==PreviewState::Jpeg); // name never hides JPEG
  std::vector<uint8_t> not_sidecar; add(not_sidecar,"./._bad",{1,2,3,4});
  assert(preview_bytes(not_sidecar).state==PreviewState::Unsupported);
  std::vector<uint8_t> directory; add(directory,"dir",photo,0040755);
  assert(preview_bytes(directory).state==PreviewState::Waiting);

  for (unsigned field : {54u,94u}) {
    std::vector<uint8_t> bad=good; memcpy(bad.data()+field,"ffffffff",8);
    assert(preview_bytes(bad).state==PreviewState::Invalid);
    bad=good; bad[field]='g'; assert(preview_bytes(bad).state==PreviewState::Invalid);
  }
  std::vector<uint8_t> bad=good; memcpy(bad.data()+94,"00000000",8);
  assert(preview_bytes(bad).state==PreviewState::Invalid);
  auto oversized=good; memcpy(oversized.data()+54,"00400000",8);
  const auto rejected=preview_bytes(oversized,iris::ARCHIVE_CAP);
  assert(rejected.state==PreviewState::Invalid && rejected.rejected_size==iris::ARCHIVE_CAP);
  assert(rejected.rejected_offset==valid.archive_offset && !rejected.jpeg);
  bad=good; bad[110+5]='X'; // overwrite a.jpg's required terminating NUL
  assert(preview_bytes(bad).state==PreviewState::Invalid);
  bad=good; bad[111]=0; assert(preview_bytes(bad).state==PreviewState::Invalid);
  bad=good; memcpy(bad.data()+94,"00001001",8); // bounded preview path length
  assert(preview_bytes(bad,8192).state==PreviewState::Unsupported);
  bad=good; bad[0]='x'; assert(preview_bytes(bad).state==PreviewState::Invalid);
  bad=good; assert(preview_bytes(bad,uint32_t(good.size()-1)).state==PreviewState::Invalid);
  std::vector<uint8_t> overflowing; add_odc(overflowing,"a.jpg",photo);
  memcpy(overflowing.data()+65,"77777777777",11);
  assert(preview_bytes(overflowing).state==PreviewState::Invalid); // 33-bit octal size
  overflowing[65]='8'; assert(preview_bytes(overflowing).state==PreviewState::Invalid);
  std::vector<uint8_t> empty; add(empty,"empty",{});
  assert(preview_bytes(empty).state==PreviewState::Unsupported);
  std::vector<uint8_t> bad_trailer=good; add(bad_trailer,"TRAILER!!!",{1});
  assert(preview_bytes(bad_trailer).state==PreviewState::Invalid);
  std::vector<uint8_t> short_jpeg; add(short_jpeg,"a.jpg",{0xff,0xd8,0xff});
  assert(preview_bytes(short_jpeg).complete); // magic only; streaming decoder must reject invalid JPEG headers
  // Mutate the envelope fields and truncate around each mutation. This checks
  // pointer invariants even when the bytes accidentally form another valid size.
  for (uint32_t at=0;at<116;++at) for (uint8_t value : {uint8_t(0),uint8_t(255)}) {
    std::vector<uint8_t> altered=good; altered[at]=value;
    (void)preview_bytes(altered);
    altered.resize(at+1); (void)preview_bytes(altered);
  }
}
int main() {
  std::vector<uint8_t> storage(4096), good;
  add(good,".",{},0040755);
  add(good,"./photo.jpg",{0xff,0xd8,0xff,0xd9});
  add(good,"./._photo.jpg",{0,5,0x16,7});
  add(good,"TRAILER!!!",{},0);
  iris::Archive a; a.data=storage.data();a.capacity=storage.size();
  assert(deliver(a,good)); assert(deliver(a,good,2));
  assert(!deliver(a,good,3,true)); assert(deliver(a,good,4));
  assert(!a.append(5,1,good.data(),10,crc32));
  assert(a.append(6,0,good.data(),10,crc32));
  assert(!a.append(6,9,good.data(),10,crc32));
  assert(!a.append(7,0,good.data(),UINT32_MAX,crc32));
  assert(deliver(a,good,8));
  const uint8_t *jpeg; uint32_t size;
  assert(!a.commit(8,good.size(),crc32(0,good.data(),good.size()),jpeg,size)); // duplicate
  std::vector<uint8_t> multi; add(multi,"a.jpg",{0xff,0xd8,0xff});add(multi,"b.jpg",{0xff,0xd8,0xff});
  assert(!deliver(a,multi,9));
  std::vector<uint8_t> video; add(video,"a.jpg",{0xff,0xd8,0xff});add(video,"b.mov",{0,0,0,24,'f','t','y','p'});
  assert(!deliver(a,video,10));
  std::vector<uint8_t> heic;add(heic,"a.heic",{0,0,0,24,'f','t','y','p','h','e','i','c'});
  assert(!deliver(a,heic,11));
  for(size_t n=1;n<120;++n) {
    std::vector<uint8_t> cut(good.begin(),good.begin()+n); assert(!deliver(a,cut,12+uint32_t(n)));
  }
  preview_test();
  puts("IRIS archive: chunk bounds, CRC, single-JPEG contract and bounded provisional previews passed");
}
