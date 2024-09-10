#include "hash_table_entry.h"

namespace Envoy::Http {

ResponseHeaderWrapper::ResponseHeaderWrapper(const ResponseHeaderWrapper& other) {
    if (other.headers_ != nullptr) {
        headers_ = ResponseHeaderMapImpl::create(other.headers_->maxHeadersKb(), other.headers_->maxHeadersCount());
        this->setHeaders(*other.headers_);
    }
}

ResponseHeaderWrapper& ResponseHeaderWrapper::operator=(ResponseHeaderWrapper other) {
    std::swap(headers_, other.headers_);
    return *this;
}

void ResponseHeaderWrapper::setHeaders(const ResponseHeaderMap& headers) {
    if (headers_ == nullptr)
        headers_ = ResponseHeaderMapImpl::create(headers.maxHeadersKb(), headers.maxHeadersCount());
    headers.iterate(collectAllHeadersCb);
}

ResponseHeaderMapImplPtr ResponseHeaderWrapper::cloneToPtr() const {
    ResponseHeaderWrapper cloneWrapper(*this);
    return std::move(cloneWrapper.headers_);
}


ResponseTrailerWrapper::ResponseTrailerWrapper(const ResponseTrailerWrapper& other) {
    if (other.trailers_ != nullptr) {
        trailers_ = ResponseTrailerMapImpl::create(other.trailers_->maxHeadersKb(), other.trailers_->maxHeadersCount());
        this->setTrailers(*other.trailers_);
    }
}

ResponseTrailerWrapper& ResponseTrailerWrapper::operator=(ResponseTrailerWrapper other) {
    std::swap(trailers_, other.trailers_);
    return *this;
}

void ResponseTrailerWrapper::setTrailers(const ResponseTrailerMap& trailers) {
    if (trailers_ == nullptr)
        trailers_ = ResponseTrailerMapImpl::create(trailers.maxHeadersKb(), trailers.maxHeadersCount());
    trailers.iterate(collectAllTrailersCb);
}

ResponseTrailerMapImplPtr ResponseTrailerWrapper::cloneToPtr() const {
    ResponseTrailerWrapper cloneWrapper(*this);
    return std::move(cloneWrapper.trailers_);
}

} // namespace Envoy::Http
