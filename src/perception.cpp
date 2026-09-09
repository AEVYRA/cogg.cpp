#include "cogg/perception.hpp"
#include <openssl/evp.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <filesystem>

namespace cogg {
namespace {
namespace fs = std::filesystem;
void need(bool ok, const char* message) { if (!ok) throw Error(message); }
struct FD { int n; explicit FD(int value):n(value) {} ~FD(){if(n>=0)::close(n);} };
std::string hash_bytes(const std::vector<std::uint8_t>& data) {
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int n=0;
    need(EVP_Digest(data.data(),data.size(),digest,&n,EVP_sha256(),nullptr)==1,"image digest failed");
    std::string out; for(unsigned int i=0;i<n;++i) {out+="0123456789abcdef"[digest[i]>>4];out+="0123456789abcdef"[digest[i]&15];} return out;
}
bool hash_string(const json& j) {
    return j.is_string() && j.get_ref<const std::string&>().size()==64 &&
        j.get_ref<const std::string&>().find_first_not_of("0123456789abcdef")==std::string::npos;
}
void directory(const std::string& path) {
    const bool created=::mkdir(path.c_str(),0700)==0;
    if (!created) need(errno==EEXIST,"cannot create media directory");
    struct stat s{};
    need(::lstat(path.c_str(),&s)==0 && S_ISDIR(s.st_mode) && s.st_uid==::getuid() && (s.st_mode&0077)==0,
         "media directory must be owned, private, and not a symlink");
    if(created) {
        auto parent=fs::path(path).parent_path();if(parent.empty())parent=".";
        FD fd(::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC));
        need(fd.n>=0&&::fsync(fd.n)==0,"media parent directory sync failed");
    }
}
std::vector<std::uint8_t> read(const std::string& path,std::size_t limit) {
    FD f(::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
    need(f.n>=0,"cannot open image/observation file"); struct stat s{};
    need(::fstat(f.n,&s)==0 && S_ISREG(s.st_mode) && s.st_size>0 && static_cast<std::uint64_t>(s.st_size)<=limit,
         "image/observation must be a nonempty bounded regular file");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(s.st_size)); std::size_t pos=0;
    while(pos<bytes.size()) {auto n=::read(f.n,bytes.data()+pos,bytes.size()-pos);if(n<0&&errno==EINTR)continue;need(n>0,"file truncated during read");pos+=static_cast<std::size_t>(n);}
    unsigned char extra=0; need(::read(f.n,&extra,1)==0,"file changed during read"); return bytes;
}
void publish(const std::string& path,const std::vector<std::uint8_t>& bytes) {
    auto pattern=path+".tmp.XXXXXX"; std::vector<char> name(pattern.begin(),pattern.end());name.push_back('\0');
    FD file(::mkstemp(name.data()));need(file.n>=0,"cannot stage media file");
    struct Cleanup {const char* path;~Cleanup(){::unlink(path);}} cleanup{name.data()};
    std::size_t pos=0;while(pos<bytes.size()){auto n=::write(file.n,bytes.data()+pos,bytes.size()-pos);if(n<0&&errno==EINTR)continue;need(n>0,"media write failed");pos+=static_cast<std::size_t>(n);}
    need(::fsync(file.n)==0,"media sync failed");
    need(::rename(name.data(),path.c_str())==0,"media publication failed");
    FD parent(::open(fs::path(path).parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC));
    need(parent.n>=0&&::fsync(parent.n)==0,"media directory sync failed");
}
std::string mime(const std::vector<std::uint8_t>& b) {
    const std::array<std::uint8_t,8> png={137,80,78,71,13,10,26,10};
    if(b.size()>=24 && std::equal(png.begin(),png.end(),b.begin())) return "image/png";
    if(b.size()>=4 && b[0]==255 && b[1]==216 && b[2]==255 && b[b.size()-2]==255 && b.back()==217) return "image/jpeg";
    throw Error("only PNG and JPEG image files are supported");
}
void descriptor(const json& j) {
    need(j.is_object()&&j.size()==4&&j.value("schema","")=="cogg:image/v1"&&j.contains("sha256")&&hash_string(j.at("sha256"))&&
         j.contains("bytes")&&j.at("bytes").is_number_integer()&&j.at("bytes")>0&&j.at("bytes")<=max_image_bytes&&
         (j.value("mime","")=="image/png"||j.value("mime","")=="image/jpeg"),"invalid image descriptor");
}
}
json import_image(const std::string& path,const std::string& dir) {
    auto bytes=read(path,max_image_bytes);auto type=mime(bytes);auto hash=hash_bytes(bytes);directory(dir);
    publish((fs::path(dir)/(hash+".image")).string(),bytes);
    return {{"schema","cogg:image/v1"},{"sha256",hash},{"mime",type},{"bytes",bytes.size()}};
}
std::vector<std::uint8_t> image_bytes(const std::string& dir,const json& image) {
    descriptor(image);auto bytes=read((fs::path(dir)/(image.at("sha256").get<std::string>()+".image")).string(),max_image_bytes);
    need(bytes.size()==image.at("bytes")&&hash_bytes(bytes)==image.at("sha256")&&mime(bytes)==image.at("mime"),"image content changed or missing");return bytes;
}
json image_context(Store& store,const std::string& subject,HttpBackend& vision,const std::string& dir,
                   millis now,millis timeout,const std::function<bool()>& cancelled) {
    auto schedule=store.schedule(subject,now);const auto& occasion=schedule.at("occasion");
    need(!occasion.is_null() && !occasion.at("id").is_null() && occasion.at("kind")=="external","image requires a queued external occasion");
    const auto image=occasion.at("payload").at("image");descriptor(image);directory(dir);
    const json binding={{"head",schedule.at("head")},{"occasion",occasion.at("id")},{"image",image},{"executor",vision.name()}};
    const auto key=execution_hash(binding), path=(fs::path(dir)/(key+".observation")).string();
    json packet;
    struct stat st{};const auto exists=::lstat(path.c_str(),&st);
    if(exists==0) {
        auto bytes=read(path,32768);auto saved=json::parse(bytes);
        need(saved.is_object()&&saved.size()==2&&saved.at("binding")==binding,"observation binding mismatch");packet=saved.at("input");
        validate_inputs(json::array({packet}));
        need(packet.at("body").at("content").at("image")==image &&
             packet.at("body").at("content").at("executor")==vision.name(),"observation identity mismatch");
    } else {
        need(errno==ENOENT,"cannot inspect observation receipt");
        const auto bytes=image_bytes(dir,image);
        const auto observed=vision.observe_image(bytes,image.at("mime").get<std::string>(),key,timeout,cancelled);
        if(observed.value("kind","")=="abstain") throw BackendFailure("vision abstained: "+observed.at("reason").get<std::string>());
        const auto description=observed.at("description").get<std::string>();
        const auto note_text="Image SHA256 "+image.at("sha256").get<std::string>()+". Vision observation: "+description;
        packet=make_input("observation","cogg:vision/v1",{{"schema","cogg:visual-observation/v1"},{"image",image},
            {"executor",vision.name()},{"provider",vision.telemetry()},{"description",description},
            {"memory_note",{{"key","image."+image.at("sha256").get<std::string>()},{"text",note_text}}}});
        validate_inputs(json::array({packet}));
        const auto serialized=json({{"binding",binding},{"input",packet}}).dump();
        publish(path,{serialized.begin(),serialized.end()});
    }
    if(cancelled&&cancelled()) throw BackendFailure("image preparation cancelled");
    const auto after=store.schedule(subject,now);
    if(after.at("head")!=binding.at("head")||after.at("occasion").is_null()||after.at("occasion").at("id")!=binding.at("occasion"))
        throw Conflict("image context became stale during observation");
    return {{"head",binding.at("head")},{"occasion",binding.at("occasion")},{"inputs",json::array({packet})}};
}
Outcome ImageActor::respond_attempt(const Attempt& a,millis timeout,const std::function<bool()>& cancelled) {
    auto outcome=actor_->respond_attempt(a,timeout,cancelled);
    if(const auto* p=std::get_if<Proposal>(&outcome)) {
        for(const auto& packet:a.present.inputs) {
            const auto& content=packet.at("body").at("content");
            if(!content.is_object()||content.value("schema","")!="cogg:visual-observation/v1")continue;
            if(p->kind!="speech"||p->text.empty())
                throw BackendFailure("describe-and-remember image request requires a visible answer or abstention",FailureKind::invalid_output);
            const auto& note=content.at("memory_note");
            bool found=false;for(const auto& n:p->notes) if(n.key==note.at("key")&&n.text==note.at("text")&&n.type=="episode"&&n.status=="active"&&n.sources.empty()&&n.covers.empty())found=true;
            if(!found)throw BackendFailure("image answer omitted the requested observation memory",FailureKind::invalid_output);
        }
    }
    return outcome;
}
}
