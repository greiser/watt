#include "app/request_tab.h"

#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/splitter.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace watt::app {

namespace {

// Workspace::append_history from several tab workers: serialize the appends.
std::mutex& history_mutex() {
  static std::mutex m;
  return m;
}

std::string now_iso() {
  const auto t = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(t);
  std::tm tm{};
  gmtime_s(&tm, &tt);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
  return s;
}

// A client / bidi body given as a JSON array: one message per element.
std::vector<std::string> split_messages(const std::string& body) {
  const auto t = trim(body);
  if (t.empty() || t.front() != '[') return {std::string(t)};
  rapidjson::Document d;
  d.Parse(t.data(), t.size());
  if (d.HasParseError() || !d.IsArray()) return {std::string(t)};
  std::vector<std::string> out;
  out.reserve(d.Size());
  for (auto& v : d.GetArray()) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    v.Accept(w);
    out.emplace_back(sb.GetString(), sb.GetSize());
  }
  return out;
}

// Whether sending r needs Workspace::http_to_send ({{vars}}, form parts, a body file, basic
// auth, disabled headers). Most re-fires need none of it, and skipping it saves ~0.2-0.4 ms.
bool needs_workspace(const SavedRequest& r) {
  // Any file reference must go through http_to_send: it is the only place the upload policy
  // (Workspace::blocked_files) is enforced and the only place these files are read.
  if (!r.form.empty() || !r.body_file.empty() || !r.body_data.empty() || !r.header_files.empty() ||
      !r.cookie_files.empty() || r.basic_auth.enabled || !r.disabled_headers.empty())
    return true;
  auto has_var = [](const std::string& s) { return s.find("{{") != std::string::npos; };
  if (has_var(r.http.url) || has_var(r.http.body)) return true;
  for (const auto& [k, v] : r.http.headers)
    if (has_var(k) || has_var(v)) return true;
  return false;
}

// Asks once per OS environment variable the request would read from outside the workspace
// (Workspace::blocked_env_vars). Expansion never resolves a non-allowed one either way; this is
// the consent prompt so the request is not sent with a literal {{process.env.X}} instead.
bool confirm_env_vars(const Workspace& ws, const SavedRequest& r, wxWindow* parent, std::string* err) {
  for (const auto& name : ws.blocked_env_vars(r)) {
    const auto ans = wxMessageBox(
        wxString::Format("This request reads the environment variable\n\n%s\n\nfrom your Windows "
                         "environment, outside the workspace. Send its value with this request?",
                         wxString::FromUTF8(name)),
        "watt", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, parent);
    std::string e;
    if (ans != wxYES || !ws.allow_env_var(name, &e)) {
      *err = e.empty() ? "Environment variable not allowed: " + name : e;
      return false;
    }
  }
  return true;
}

bool grpc_has_vars(const SavedRequest& r) {
  auto has_var = [](const std::string& s) { return s.find("{{") != std::string::npos; };
  if (has_var(r.grpc_target.address) || has_var(r.grpc_target.authority) || has_var(r.grpc_method) ||
      has_var(r.grpc_body))
    return true;
  for (const auto* md : {&r.grpc_opts.metadata, &r.grpc_opts.rpc_metadata, &r.grpc_opts.reflect_metadata})
    for (const auto& [k, v] : *md)
      if (has_var(k) || has_var(v)) return true;
  return false;
}

std::string url_tail(const std::string& url) {
  auto p = url.find("://");
  std::string s = p == std::string::npos ? url : url.substr(p + 3);
  if (s.size() > 40) s = "..." + s.substr(s.size() - 37);
  return s;
}

}  // namespace

// ------------------------------------------------------------------ worker

struct TabWorker : std::enable_shared_from_this<TabWorker> {
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  bool stop = false;

  std::mutex post_m;
  RequestTab* tab = nullptr;  // null once the tab is gone

  std::mutex http_m;
  std::unique_ptr<HttpClient> http;  // created and used on the worker thread

  std::mutex call_m;
  std::shared_ptr<GrpcCall> call;
  std::atomic<uint64_t> cancelled_gen{0};

  std::mutex msg_m;
  std::vector<GrpcMessageEvent> msgs;
  bool msg_posted = false;
  uint64_t msg_gen = 0;  // messages of other generations are dropped

  void start() {
    std::thread([self = shared_from_this()] { self->run(); }).detach();
  }
  void run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lk(m);
        cv.wait(lk, [&] { return stop || !jobs.empty(); });
        if (stop) break;
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      job();
    }
    std::lock_guard lk(http_m);
    http.reset();
  }
  void push(std::function<void()> job) {
    {
      std::lock_guard lk(m);
      jobs.push_back(std::move(job));
    }
    cv.notify_one();
  }
  void shutdown() {
    cancel_all();
    {
      std::lock_guard lk(post_m);
      tab = nullptr;
    }
    {
      std::lock_guard lk(m);
      stop = true;
      jobs.clear();
    }
    cv.notify_one();
  }
  // Runs fn(tab) on the UI thread if the tab still exists.
  template <class F>
  void post(F&& fn) {
    std::lock_guard lk(post_m);
    if (!tab) return;
    RequestTab* t = tab;
    t->CallAfter([t, fn = std::forward<F>(fn)]() mutable { fn(*t); });
  }
  HttpClient& http_client() {
    std::lock_guard lk(http_m);
    if (!http) http = std::make_unique<HttpClient>();
    return *http;
  }
  void cancel(uint64_t gen) {
    cancelled_gen.store(gen);
    {
      std::lock_guard lk(http_m);
      if (http) http->cancel();
    }
    std::lock_guard lk(call_m);
    if (call) call->cancel();
  }
  void cancel_all() { cancel(UINT64_MAX); }
  bool is_cancelled(uint64_t gen) const {
    const auto c = cancelled_gen.load();
    return c == UINT64_MAX || c >= gen;
  }
  // gRPC message from the engine thread: queue it, post at most one flush per UI turn.
  void on_message(uint64_t gen, const GrpcMessageEvent& e) {
    bool post_now = false;
    {
      std::lock_guard lk(msg_m);
      if (gen != msg_gen) return;
      msgs.push_back(e);
      if (!msg_posted) post_now = msg_posted = true;
    }
    if (post_now) post([gen](RequestTab& t) { t.on_grpc_messages(gen); });
  }
  std::vector<GrpcMessageEvent> take_messages() {
    std::lock_guard lk(msg_m);
    msg_posted = false;
    return std::exchange(msgs, {});
  }
};

// ------------------------------------------------------------------ tab

RequestTab::RequestTab(wxWindow* parent, TabContext ctx, SavedRequest req, bool preview)
    : wxPanel(parent, wxID_ANY), ctx_(std::move(ctx)), req_(std::move(req)), preview_(preview) {
  split_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxSP_LIVE_UPDATE | wxSP_NOBORDER | wxSP_THIN_SASH);
  split_->SetSashGravity(0.5);
  split_->SetMinimumPaneSize(FromDIP(240));
  resp_ = make_response_view(split_);
  RequestEditorPanel* ed = ensure_editor(req_.kind);
  split_->SplitVertically(ed->window(), resp_->window());
  auto* sz = new wxBoxSizer(wxVERTICAL);
  sz->Add(split_, 1, wxEXPAND);
  SetSizer(sz);
  // Keep the split at 50% on first layout (SplitVertically before the first size event uses 0).
  Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
    e.Skip();
    if (!split_sized_ && GetClientSize().x > 0) {
      split_sized_ = true;
      CallAfter([this] { split_->SetSashPosition(split_->GetClientSize().x / 2); });
    }
  });
  resp_->clear();

  worker_ = std::make_shared<TabWorker>();
  worker_->tab = this;
  worker_->start();
}

RequestTab::~RequestTab() { worker_->shutdown(); }

RequestEditorPanel* RequestTab::editor() const {
  return req_.kind == RequestKind::Http ? static_cast<RequestEditorPanel*>(http_)
                                        : static_cast<RequestEditorPanel*>(grpc_);
}

RequestEditorPanel* RequestTab::ensure_editor(RequestKind kind) {
  RequestEditorPanel* ed;
  if (kind == RequestKind::Http) {
    if (!http_) http_ = make_http_editor(split_, this);
    ed = http_;
  } else {
    if (!grpc_) grpc_ = make_grpc_editor(split_, this);
    ed = grpc_;
  }
  loading_ = true;
  ed->load(req_);
  loading_ = false;
  return ed;
}

void RequestTab::show_editor(RequestKind kind) {
  wxWindow* old = editor()->window();
  req_.kind = kind;
  RequestEditorPanel* ed = ensure_editor(kind);
  if (old != ed->window()) {
    split_->ReplaceWindow(old, ed->window());
    old->Hide();
    ed->window()->Show();
  }
}

void RequestTab::store() const { editor()->store(req_); }

const SavedRequest& RequestTab::request() const {
  store();
  return req_;
}

std::wstring RequestTab::title() const {
  std::string t;
  if (!req_.name.empty()) {
    t = req_.name;
  } else if (req_.kind == RequestKind::Http) {
    t = req_.http.url.empty() ? "Untitled" : url_tail(req_.http.url);
  } else {
    const auto& m = req_.grpc_method;
    const auto s = m.rfind('/');
    t = m.empty() ? "Untitled gRPC" : m.substr(s == std::string::npos ? 0 : s + 1);
  }
  std::string prefix = req_.kind == RequestKind::Http ? req_.http.method : std::string("gRPC");
  return wxString::FromUTF8(prefix + " " + t).ToStdWstring() + (dirty_ ? L" \u25CF" : L"");
}

void RequestTab::make_permanent() {
  if (!preview_) return;
  preview_ = false;
  if (on_title_changed) on_title_changed(this);
}

void RequestTab::set_request(SavedRequest req, bool preview) {
  if (in_flight_) cancel();
  wxWindow* old = editor()->window();
  req_ = std::move(req);
  preview_ = preview;
  dirty_ = false;
  RequestEditorPanel* ed = ensure_editor(req_.kind);
  if (old != ed->window()) {
    split_->ReplaceWindow(old, ed->window());
    old->Hide();
    ed->window()->Show();
  }
  resp_->clear();
  if (on_title_changed) on_title_changed(this);
}

bool RequestTab::save(Workspace& ws, std::string* err) {
  store();
  if (req_.name.empty()) {
    if (req_.kind == RequestKind::Http) {
      req_.name = url_tail(req_.http.url);
    } else {
      const auto s = req_.grpc_method.rfind('/');
      req_.name = s == std::string::npos ? req_.grpc_method : req_.grpc_method.substr(s + 1);
    }
    for (char& c : req_.name)
      if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    if (req_.name.empty()) req_.name = "request";
  }
  if (req_.file.empty() && !save_folder_.empty())
    req_.file = save_folder_ / wxString::FromUTF8(req_.name + ".req.json").ToStdWstring();
  if (!ws.save_request(req_, err)) return false;
  dirty_ = false;
  preview_ = false;
  if (on_title_changed) on_title_changed(this);
  return true;
}

void RequestTab::focus_url() { editor()->focus_url(); }

void RequestTab::file_moved(const std::filesystem::path& to) {
  req_.file = to;
  if (on_title_changed) on_title_changed(this);
}

// ------------------------------------------------------------------ EditorHost

void RequestTab::changed() {
  if (loading_) return;
  editor()->store_head(req_);  // cheap: name, method, url, target (never the body)
  dirty_ = true;
  preview_ = false;
  if (on_title_changed) on_title_changed(this);
}

void RequestTab::switch_kind(RequestKind kind) {
  if (kind == req_.kind) return;
  if (in_flight_) cancel();
  store();
  show_editor(kind);
  resp_->clear();
  dirty_ = true;
  if (on_title_changed) on_title_changed(this);
}

const Workspace* RequestTab::workspace() const { return ctx_.workspace ? ctx_.workspace() : nullptr; }

void RequestTab::send() {
  const auto click = Clock::now();
  if (in_flight_) cancel();
  store();
  if (req_.kind == RequestKind::Http)
    send_http(click);
  else
    send_grpc(click);
}

void RequestTab::send_http(Clock::time_point click) {
  Workspace* ws = ctx_.workspace ? ctx_.workspace() : nullptr;
  if (req_.http.url.empty()) {
    resp_->show_error("Enter a URL first.");
    return;
  }
  // Files from outside the workspace and OS environment variables need the user's consent
  // (Workspace::blocked_files / blocked_env_vars). http_to_send() refuses them regardless, so
  // this is the prompt, not the gate. Only requests that go through http_to_send can reach them.
  if (ws && needs_workspace(req_)) {
    if (std::string err; !confirm_env_vars(*ws, req_, this, &err)) {
      resp_->show_error(err);
      return;
    }
    for (const auto& p : ws->blocked_files(req_)) {
      const auto ans = wxMessageBox(
          wxString::Format("This request wants to upload\n\n%s\n\nfrom outside the workspace. Allow it?",
                           p.wstring()),
          "watt", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
      std::string err;
      if (ans != wxYES || !ws->allow_file(p, &err)) {
        resp_->show_error(err.empty() ? "Upload not allowed: " + p.string() : err);
        return;
      }
    }
  }
  const uint64_t gen = ++gen_;
  in_flight_ = true;
  sent_at_ = click;
  // The job is queued before any UI update, so the worker starts while the UI repaints.
  auto saved = std::make_shared<const SavedRequest>(req_);
  auto wsc = ws ? std::make_shared<Workspace>(*ws) : nullptr;
  auto w = worker_;
  w->push([w, gen, click, saved, wsc] {
    if (w->is_cancelled(gen)) return;
    auto sent = std::make_shared<SentHttp>();
    if (wsc && needs_workspace(*saved)) {
      std::string err;
      auto r = wsc->http_to_send(*saved, &err);
      if (!r) {
        w->post([gen, err](RequestTab& t) { t.on_error(gen, err); });
        return;
      }
      sent->request = std::move(*r);
    } else {
      sent->request = saved->http;
    }
    HttpClient& client = w->http_client();
    if (w->is_cancelled(gen)) return;
    // Progress: at most every 50 ms.
    uint64_t got = 0;
    auto last = Clock::now();
    auto on_chunk = [&](const char*, size_t n) {
      got += n;
      const auto now = Clock::now();
      if (ms_between(last, now) >= 50) {
        last = now;
        w->post([gen, got](RequestTab& t) {
          if (t.gen_ == gen && t.in_flight_) t.resp_->show_progress(got);
        });
      }
    };
    const auto start = Clock::now();
    const double ui_queue = ms_between(click, start);
    auto resp = std::make_shared<HttpResponse>(client.send(sent->request, on_chunk));
    resp->timing.queue_ms += ui_queue;
    resp->timing.total_ms += ui_queue;
    std::shared_ptr<const HttpResponse> cresp = resp;
    std::shared_ptr<const SentHttp> csent = sent;
    w->post([gen, cresp, csent](RequestTab& t) { t.on_http_done(gen, cresp, csent); });

    if (wsc && !(resp->error == "cancelled")) {
      HistoryEntry e;
      e.when_iso = now_iso();
      e.kind = RequestKind::Http;
      e.summary = saved->http.method + " " + saved->http.url;
      e.status = resp->status;
      e.total_ms = resp->timing.total_ms;
      e.request = *saved;
      {
        std::lock_guard lk(history_mutex());
        wsc->append_history(e);
      }
      w->post([e = std::move(e)](RequestTab& t) {
        if (t.ctx_.history_added) t.ctx_.history_added(e);
      });
    }
  });
  editor()->set_in_flight(true);
  resp_->show_sending(RequestKind::Http);
}

void RequestTab::send_grpc(Clock::time_point click) {
  if (req_.grpc_method.empty() || req_.grpc_target.address.empty()) {
    resp_->show_error("Choose a server address and a method first.");
    return;
  }
  Workspace* ws = ctx_.workspace ? ctx_.workspace() : nullptr;
  if (ws && grpc_has_vars(req_)) {
    if (std::string err; !confirm_env_vars(*ws, req_, this, &err)) {
      resp_->show_error(err);
      return;
    }
  }
  const uint64_t gen = ++gen_;
  in_flight_ = true;
  sent_at_ = click;
  {
    std::lock_guard lk(worker_->msg_m);
    worker_->msgs.clear();
    worker_->msg_posted = false;
    worker_->msg_gen = gen;
  }

  auto saved = std::make_shared<const SavedRequest>(req_);
  auto wsc = ws ? std::make_shared<Workspace>(*ws) : nullptr;
  const bool interactive = grpc_ && grpc_->interactive();
  auto w = worker_;
  auto client = ctx_.grpc();
  w->push([w, gen, click, saved, wsc, interactive, client] {
    if (w->is_cancelled(gen)) return;
    const SavedRequest r = wsc && grpc_has_vars(*saved) ? wsc->expanded(*saved) : *saved;
    const auto start = Clock::now();
    const double ui_queue = ms_between(click, start);
    std::shared_ptr<GrpcCall> call(client->start_call(
        r.grpc_target, r.grpc_method, r.grpc_opts,
        [w, gen](const GrpcMessageEvent& e) { w->on_message(gen, e); }));
    {
      std::lock_guard lk(w->call_m);
      w->call = call;
      if (w->is_cancelled(gen)) call->cancel();
    }
    if (!call->done()) {
      if (interactive) {
        if (!trim(r.grpc_body).empty()) call->send(r.grpc_body);
      } else {
        for (const auto& m : split_messages(r.grpc_body)) {
          std::string err;
          if (!call->send(m, &err)) {
            call->cancel();
            w->post([gen, err](RequestTab& t) { t.on_error(gen, "Request message: " + err); });
            break;
          }
        }
        call->close_send();
      }
    }
    auto resp = std::make_shared<GrpcResponse>(call->wait());
    {
      std::lock_guard lk(w->call_m);
      w->call.reset();
    }
    resp->timing.queue_ms += ui_queue;
    resp->timing.total_ms += ui_queue;
    std::shared_ptr<const GrpcResponse> cresp = resp;
    w->post([gen, cresp](RequestTab& t) { t.on_grpc_done(gen, cresp); });

    if (wsc && resp->status_name != "CANCELLED") {
      HistoryEntry e;
      e.when_iso = now_iso();
      e.kind = RequestKind::Grpc;
      e.summary = saved->grpc_method + " @ " + saved->grpc_target.address;
      e.status = resp->status_code;
      e.total_ms = resp->timing.total_ms;
      e.request = *saved;
      {
        std::lock_guard lk(history_mutex());
        wsc->append_history(e);
      }
      w->post([e = std::move(e)](RequestTab& t) {
        if (t.ctx_.history_added) t.ctx_.history_added(e);
      });
    }
  });
  editor()->set_in_flight(true);
  resp_->show_sending(RequestKind::Grpc);
}

void RequestTab::cancel() {
  if (!in_flight_) return;
  worker_->cancel(gen_);
  const double after = ms_between(sent_at_, Clock::now());
  finish(gen_);
  resp_->show_cancelled(after);
}

void RequestTab::finish(uint64_t gen) {
  if (gen != gen_ || !in_flight_) return;
  in_flight_ = false;
  editor()->set_in_flight(false);
}

void RequestTab::on_http_done(uint64_t gen, std::shared_ptr<const HttpResponse> r,
                              std::shared_ptr<const SentHttp> sent) {
  if (gen != gen_ || !in_flight_) return;
  finish(gen);
  resp_->show_http(std::move(r), std::move(sent));
}

void RequestTab::on_grpc_messages(uint64_t gen) {
  auto batch = worker_->take_messages();
  if (gen != gen_ || !in_flight_ || batch.empty()) return;
  resp_->append_grpc_messages(batch);
}

void RequestTab::on_grpc_done(uint64_t gen, std::shared_ptr<const GrpcResponse> r) {
  if (gen != gen_ || !in_flight_) return;
  on_grpc_messages(gen);
  finish(gen);
  resp_->show_grpc(std::move(r));
}

void RequestTab::on_error(uint64_t gen, const std::string& msg) {
  if (gen != gen_ || !in_flight_) return;
  finish(gen);
  resp_->show_error(msg);
}

void RequestTab::load_schema(const SavedRequest& r) {
  // Not on the tab worker: an interactive stream may keep it busy for minutes.
  auto w = worker_;
  auto client = ctx_.grpc();
  Workspace* ws = ctx_.workspace ? ctx_.workspace() : nullptr;
  auto req = std::make_shared<SavedRequest>(ws ? ws->expanded(r) : r);
  std::thread([w, client, req] {
    GrpcSchema s;
    if (req->proto_files.empty()) {
      Metadata md = req->grpc_opts.metadata;
      md.insert(md.end(), req->grpc_opts.reflect_metadata.begin(), req->grpc_opts.reflect_metadata.end());
      s = client->load_via_reflection(req->grpc_target, 5000, md);
    } else {
      s = client->load_proto_files(req->proto_files, req->import_dirs);
    }
    auto cs = std::make_shared<const GrpcSchema>(std::move(s));
    w->post([cs](RequestTab& t) {
      if (t.grpc_) t.grpc_->set_schema(cs);
    });
  }).detach();
}

void RequestTab::preconnect(const GrpcTarget& target) {
  Workspace* ws = ctx_.workspace ? ctx_.workspace() : nullptr;
  GrpcTarget t = target;
  if (ws) t.address = ws->expand(t.address);
  ctx_.grpc()->preconnect(t);
}

bool RequestTab::stream_send(const std::string& json, std::string* error) {
  std::shared_ptr<GrpcCall> call;
  {
    std::lock_guard lk(worker_->call_m);
    call = worker_->call;
  }
  if (!call || call->done()) {
    if (error) *error = "No stream is open.";
    return false;
  }
  return call->send(json, error);
}

void RequestTab::stream_close() {
  std::shared_ptr<GrpcCall> call;
  {
    std::lock_guard lk(worker_->call_m);
    call = worker_->call;
  }
  if (call) call->close_send();
}

}  // namespace watt::app
