#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace carto::studio {
struct TextureUploadIdentity {
    std::string role, digest;
    std::uint32_t width = 0, height = 0;
    bool operator==(const TextureUploadIdentity&) const = default;
};

// Cancellation withdraws publication intent, not GPU ownership. Only a
// completed submission may release its command buffer and staging storage.
class TextureUploadTicket {
public:
    explicit TextureUploadTicket(TextureUploadIdentity identity) : identity_(std::move(identity)) {}
    const TextureUploadIdentity& identity() const { return identity_; }
    bool mark_submitted() {
        if (submitted_ || complete_) return false;
        submitted_ = true;
        return true;
    }
    bool observe_completion() {
        if (!submitted_) return false;
        complete_ = true;
        return true;
    }
    void cancel() { cancelled_ = true; }
    bool cancelled() const { return cancelled_; }
    bool may_release_submission() const { return !submitted_ || complete_; }
    bool may_discard_unclaimed() const { return submitted_ && complete_; }
    bool may_publish(const TextureUploadIdentity& current) const {
        return submitted_ && complete_ && !cancelled_ && identity_ == current;
    }
private:
    TextureUploadIdentity identity_;
    bool submitted_ = false, complete_ = false, cancelled_ = false;
};

// A role can return to an already-published image while another source for the
// same role is still uploading. Cancellation never changes GPU ownership.
template<class Uploads>
void cancel_uploads_for_role(Uploads& uploads, std::string_view role) noexcept {
    for (auto& upload : uploads)
        if (upload.ticket.identity().role == role) upload.ticket.cancel();
}

// Completed results are only a cache. Under capacity pressure, discard the
// oldest unclaimed result so an abandoned role cannot block all later roles.
// The caller checks for a matching current request before invoking this.
template<class Uploads, class Destroy>
bool reclaim_completed_upload(Uploads& uploads, Destroy&& destroy) {
    for (auto it = uploads.begin(); it != uploads.end(); ++it) {
        if (!it->ticket.may_discard_unclaimed()) continue;
        it->ticket.cancel();
        destroy(*it);
        uploads.erase(it);
        return true;
    }
    return false;
}
}
