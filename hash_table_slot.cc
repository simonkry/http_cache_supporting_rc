#include "hash_table_slot.h"

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

void ResponseHeaderWrapper::setHeaders(const ResponseHeaderMap & headers) {
    if (headers_ == nullptr)
        headers_ = ResponseHeaderMapImpl::create(headers.maxHeadersKb(), headers.maxHeadersCount());
    headers.iterate(collectAllHeadersCB);
}

ResponseHeaderMapImplPtr ResponseHeaderWrapper::cloneToPtr() const {
    ResponseHeaderWrapper ptrCloneWrapper;
    ptrCloneWrapper.setHeaders(*headers_);
    return std::move(ptrCloneWrapper.headers_);
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

void ResponseTrailerWrapper::setTrailers(const ResponseTrailerMap & trailers) {
    if (trailers_ == nullptr)
        trailers_ = ResponseTrailerMapImpl::create(trailers.maxHeadersKb(), trailers.maxHeadersCount());
    trailers.iterate(collectAllTrailersCB);
}

ResponseTrailerMapImplPtr ResponseTrailerWrapper::cloneToPtr() const {
    ResponseTrailerWrapper ptrCloneWrapper;
    ptrCloneWrapper.setTrailers(*trailers_);
    return std::move(ptrCloneWrapper.trailers_);
}


HashTableSlot::HashTableSlot(const HashTableSlot & other)
                    :   host_url_(other.host_url_),
                        header_wrapper_(other.header_wrapper_),
                        trailer_wrapper_(other.trailer_wrapper_),
                        state_(other.state_) {
    std::for_each(other.data_.begin(), other.data_.end(),
                  [&](const auto& dataBatch) { data_.emplace_back(dataBatch.toString()); });
}

HashTableSlot& HashTableSlot::operator=(HashTableSlot other) {
    std::swap(host_url_, other.host_url_);
    std::swap(header_wrapper_, other.header_wrapper_);
    std::swap(data_, other.data_);
    std::swap(trailer_wrapper_, other.trailer_wrapper_);
    std::swap(state_, other.state_);
    return *this;
}

} // namespace Envoy::Http
