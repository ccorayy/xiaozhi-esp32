#pragma once

// ---------------------------------------------------------------------------
// SD kart icin cihaz uzerinde kucuk bir web sunucusu.
//
// Neden: kullanicinin kart okuyucusu yok, PC'den karta dosya atmanin baska
// yolu kalmiyor. USB MSC olmaz - bu kartta USB-Serial/JTAG ile USB-OTG ayni
// PHY'yi paylasiyor, MSC'ye gecmek kabloyla flash'i feda etmek demek.
//
// Sunucu varsayilan olarak KAPALI; menudeki SD uygulamasindan aciliyor.
// Kimlik dogrulamasi yok, o yuzden surekli acik durmasin diye boyle.
//
// Yol haritasi: /sdcard bir kez dolduktan sonra tuketiciler gelecek
// (LVGL dosya sistemi surucusu ile SD'den gorsel, MCP dosya araci ile
// asistanin not tutmasi).
// ---------------------------------------------------------------------------

#include <dirent.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

class SdWebServer {
public:
    explicit SdWebServer(const char* mount_point) : mount_(mount_point) {}

    bool running() const { return server_ != nullptr; }

    bool Start() {
        if (server_ != nullptr) {
            return true;
        }
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = 80;
        config.stack_size = 8192;      // dosya yazma tamponu yigina degil, ayri
        config.max_uri_handlers = 8;
        config.lru_purge_enable = true;

        esp_err_t err = httpd_start(&server_, &config);
        if (err != ESP_OK) {
            server_ = nullptr;
            ESP_LOGE(kTag, "httpd_start: %s", esp_err_to_name(err));
            return false;
        }

        Register("/", HTTP_GET, IndexHandler);
        Register("/api/list", HTTP_GET, ListHandler);
        Register("/api/upload", HTTP_POST, UploadHandler);
        Register("/api/delete", HTTP_GET, DeleteHandler);
        Register("/dl", HTTP_GET, DownloadHandler);
        ESP_LOGI(kTag, "Dosya sunucusu acildi (port 80)");
        return true;
    }

    void Stop() {
        if (server_ == nullptr) {
            return;
        }
        httpd_stop(server_);
        server_ = nullptr;
        ESP_LOGI(kTag, "Dosya sunucusu kapatildi");
    }

private:
    static constexpr const char* kTag = "SdWeb";
    static constexpr size_t kChunk = 4096;
    static constexpr size_t kMaxNameLen = 64;

    httpd_handle_t server_ = nullptr;
    std::string mount_;

    void Register(const char* path, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
        httpd_uri_t uri = {};
        uri.uri = path;
        uri.method = method;
        uri.handler = handler;
        uri.user_ctx = this;
        httpd_register_uri_handler(server_, &uri);
    }

    static SdWebServer* Self(httpd_req_t* req) {
        return static_cast<SdWebServer*>(req->user_ctx);
    }

    // ------------------------------------------------------------------
    // Dosya adi guvenligi
    // ------------------------------------------------------------------
    // Tarayicidan gelen ada guvenmiyoruz: dizin ayraci ya da ".." iceren
    // bir ad /sdcard disina yazdirabilir.
    static bool NameIsSafe(const std::string& name) {
        if (name.empty() || name.size() > kMaxNameLen) {
            return false;
        }
        if (name == "." || name == ".." || name.find("..") != std::string::npos) {
            return false;
        }
        return name.find('/') == std::string::npos && name.find('\\') == std::string::npos;
    }

    // %20 gibi kacislari cozer; sorgu dizesinden ad okumak icin.
    static std::string UrlDecode(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); i++) {
            if (in[i] == '%' && i + 2 < in.size()) {
                out.push_back(static_cast<char>(strtol(in.substr(i + 1, 2).c_str(), nullptr, 16)));
                i += 2;
            } else if (in[i] == '+') {
                out.push_back(' ');
            } else {
                out.push_back(in[i]);
            }
        }
        return out;
    }

    // Basarisizsa istemciye hatayi kendi yazar ve bos dondurur.
    static std::string QueryName(httpd_req_t* req) {
        char query[160];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name yok");
            return "";
        }
        char value[128];
        if (httpd_query_key_value(query, "name", value, sizeof(value)) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name yok");
            return "";
        }
        std::string name = UrlDecode(value);
        if (!NameIsSafe(name)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gecersiz ad");
            return "";
        }
        return name;
    }

    std::string PathOf(const std::string& name) const { return mount_ + "/" + name; }

    // ------------------------------------------------------------------
    // Sayfalar
    // ------------------------------------------------------------------
    static esp_err_t IndexHandler(httpd_req_t* req) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
    }

    static esp_err_t ListHandler(httpd_req_t* req) {
        auto* self = Self(req);
        httpd_resp_set_type(req, "application/json");

        DIR* dir = opendir(self->mount_.c_str());
        if (dir == nullptr) {
            return httpd_resp_sendstr(req, "[]");
        }
        std::string json = "[";
        struct dirent* entry = nullptr;
        bool first = true;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            struct stat st = {};
            long size = 0;
            if (stat(self->PathOf(entry->d_name).c_str(), &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    continue;  // FAT'te alt dizin olabilir; simdilik kok dizin
                }
                size = static_cast<long>(st.st_size);
            }
            if (!first) {
                json += ",";
            }
            first = false;
            json += "{\"n\":\"";
            json += JsonEscape(entry->d_name);
            json += "\",\"s\":" + std::to_string(size) + "}";
        }
        closedir(dir);
        json += "]";
        return httpd_resp_sendstr(req, json.c_str());
    }

    static std::string JsonEscape(const char* text) {
        std::string out;
        for (const char* p = text; *p != '\0'; p++) {
            if (*p == '"' || *p == '\\') {
                out.push_back('\\');
            }
            out.push_back(*p);
        }
        return out;
    }

    // Govde ham dosya icerigi; adi sorgu dizesinde geliyor. Multipart yerine
    // bunu sectik: tarayici tarafinda fetch(body: file) bir satir, cihaz
    // tarafinda ayristirici yazmaya gerek kalmiyor.
    static esp_err_t UploadHandler(httpd_req_t* req) {
        auto* self = Self(req);
        std::string name = QueryName(req);
        if (name.empty()) {
            return ESP_FAIL;
        }

        std::string path = self->PathOf(name);
        FILE* file = fopen(path.c_str(), "wb");
        if (file == nullptr) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "dosya acilamadi");
            return ESP_FAIL;
        }

        std::vector<char> buffer(kChunk);
        int remaining = req->content_len;
        bool ok = true;
        while (remaining > 0) {
            int wanted = remaining < static_cast<int>(kChunk) ? remaining : static_cast<int>(kChunk);
            int received = httpd_req_recv(req, buffer.data(), wanted);
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            if (received <= 0) {
                ok = false;
                break;
            }
            if (fwrite(buffer.data(), 1, received, file) != static_cast<size_t>(received)) {
                ok = false;
                break;
            }
            remaining -= received;
        }
        fclose(file);

        if (!ok) {
            unlink(path.c_str());  // yarim dosya birakma
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "yazma hatasi");
            return ESP_FAIL;
        }
        ESP_LOGI(kTag, "Yuklendi: %s (%d bayt)", name.c_str(), req->content_len);
        return httpd_resp_sendstr(req, "ok");
    }

    static esp_err_t DeleteHandler(httpd_req_t* req) {
        auto* self = Self(req);
        std::string name = QueryName(req);
        if (name.empty()) {
            return ESP_FAIL;
        }
        if (unlink(self->PathOf(name).c_str()) != 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "silinemedi");
            return ESP_FAIL;
        }
        ESP_LOGI(kTag, "Silindi: %s", name.c_str());
        return httpd_resp_sendstr(req, "ok");
    }

    static esp_err_t DownloadHandler(httpd_req_t* req) {
        auto* self = Self(req);
        std::string name = QueryName(req);
        if (name.empty()) {
            return ESP_FAIL;
        }
        FILE* file = fopen(self->PathOf(name).c_str(), "rb");
        if (file == nullptr) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "yok");
            return ESP_FAIL;
        }
        httpd_resp_set_type(req, "application/octet-stream");
        std::string disposition = "attachment; filename=\"" + name + "\"";
        httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());

        std::vector<char> buffer(kChunk);
        size_t read = 0;
        while ((read = fread(buffer.data(), 1, kChunk, file)) > 0) {
            if (httpd_resp_send_chunk(req, buffer.data(), read) != ESP_OK) {
                fclose(file);
                return ESP_FAIL;
            }
        }
        fclose(file);
        return httpd_resp_send_chunk(req, nullptr, 0);
    }

    // Tek sayfa, dis kaynak yok (cihaz internete acilmiyor, CDN cekemez).
    static constexpr const char* kIndexHtml = R"HTML(<!doctype html>
<html lang="tr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Agon SD Kart</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee}
h1{font-size:18px;margin:0 0 12px}
#drop{border:2px dashed #555;border-radius:10px;padding:24px;text-align:center;color:#aaa}
#drop.on{border-color:#0a84ff;color:#0a84ff}
table{width:100%;border-collapse:collapse;margin-top:16px;font-size:14px}
td,th{text-align:left;padding:6px 4px;border-bottom:1px solid #333}
td.s{text-align:right;color:#888;white-space:nowrap}
a{color:#0a84ff;text-decoration:none}
button{background:#333;color:#eee;border:0;border-radius:6px;padding:4px 10px;cursor:pointer}
#bar{height:4px;background:#0a84ff;width:0;transition:width .2s;margin-top:8px}
#msg{margin-top:8px;color:#888;font-size:13px}
</style></head><body>
<h1>Agon &mdash; SD Kart</h1>
<div id="drop">Dosyayi buraya surukle<br><br><input type="file" id="f" multiple></div>
<div id="bar"></div><div id="msg"></div>
<table id="t"><thead><tr><th>Dosya</th><th class="s">Boyut</th><th></th></tr></thead><tbody></tbody></table>
<script>
const q=s=>document.querySelector(s), tb=q('#t tbody'), bar=q('#bar'), msg=q('#msg');
const human=n=>n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(1)+' MB';
async function list(){
  const r=await fetch('/api/list'); const files=await r.json();
  tb.innerHTML=files.map(f=>`<tr><td><a href="/dl?name=${encodeURIComponent(f.n)}">${f.n}</a></td>
    <td class="s">${human(f.s)}</td><td><button data-n="${f.n}">Sil</button></td></tr>`).join('')
    || '<tr><td colspan="3" style="color:#888">Kart bos</td></tr>';
  tb.querySelectorAll('button').forEach(b=>b.onclick=async()=>{
    await fetch('/api/delete?name='+encodeURIComponent(b.dataset.n)); list();
  });
}
async function upload(files){
  for(let i=0;i<files.length;i++){
    const f=files[i]; msg.textContent=`Yukleniyor: ${f.name}`; bar.style.width=((i)/files.length*100)+'%';
    const r=await fetch('/api/upload?name='+encodeURIComponent(f.name),{method:'POST',body:f});
    if(!r.ok){msg.textContent=`Hata: ${f.name}`;bar.style.width='0';return;}
  }
  bar.style.width='100%'; msg.textContent='Tamam'; setTimeout(()=>bar.style.width='0',800); list();
}
q('#f').onchange=e=>upload(e.target.files);
const d=q('#drop');
d.ondragover=e=>{e.preventDefault();d.classList.add('on')};
d.ondragleave=()=>d.classList.remove('on');
d.ondrop=e=>{e.preventDefault();d.classList.remove('on');upload(e.dataTransfer.files)};
list();
</script></body></html>)HTML";
};
