//
// Created by arseny on 31/01/2020.
//
#include "BaseHttp.h"
#include "codes/HttpResultCodes.h"
#include "codes/HttpStatusCodes.h"

#include <fmt/format.h>
#include <boost/asio/connect.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <iostream>
#include <boost/asio/read.hpp>
#include <regex>
#include <diagnostic/Diagnostics.h>

namespace http::base {

BaseHttp::BaseHttp() {
  diagnostics::Diagnostics::Initialize();
  spdlog::trace("BaseHttp::BaseHttp() called.");
}

BaseHttp::~BaseHttp() = default;

codes::HttpResultCodes BaseHttp::StartReadingProcess(std::string_view url, std::string_view app) {
  spdlog::debug("BaseHttp::StartReadingProcess called. Url is '{}'; app is '{}'.", url, app);

  if (url.empty()) {
    spdlog::warn("BaseHttp::StartReadingProcess got empty url. exiting with {} code.",
    	codes::HttpResultCodes::InvalidRequest_host);
	return codes::HttpResultCodes::InvalidRequest_host;
  }

  boost::system::error_code error;
  try {
	tcp::socket socket = Connect(url.data());

	// Send the request.
	buffer request;
	GenerateRequest(request, url.data(), app.data());
	boost::asio::write(socket, request);

	// Read the response status line. The response streambuf will
	// automatically grow to accommodate the entire line. The growth may be
	// limited by passing a maximum size to the streambuf constructor.
	buffer response;
	boost::asio::read_until(socket, response, kRequestDelimiter.data(), error);

	if (error != nullptr) {
	  spdlog::warn("BaseHttp::StartReadingProcess got {} error on request.", error);
	  spdlog::info("BaseHttp::StartReadingProcess trying print response...");
	  LogBufferStream(response);
	  spdlog::info("BaseHttp::StartReadingProcess done. Exiting with {} code.",
	  	codes::HttpResultCodes::ProcessingError);
	  return codes::HttpResultCodes::ProcessingError;
	}
    spdlog::trace("BaseHttp::StartReadingProcess error code is 0.");

	// Check that response is OK.
	std::istream response_stream(&response);

	std::string http_version;
	response_stream >> http_version;
    spdlog::info("BaseHttp::StartReadingProcess Response returned with http version '{}'.", http_version);

	unsigned int status_code;
	response_stream >> status_code;
    spdlog::info("BaseHttp::StartReadingProcess Response returned with status code '{}'.", status_code);

	std::string status_message;
	std::getline(response_stream, status_message);

	if (!response_stream || http_version.substr(0, 5) != "HTTP/") {
	  spdlog::warn("BaseHttp::StartReadingProcess got invalid response.");
	  LogBufferStream(response);
	  return codes::HttpResultCodes::InvalidResponse;
	}
    spdlog::trace("BaseHttp::StartReadingProcess response is valid.");

	if (status_code == codes::HttpStatusCodes::Moved) {
	  spdlog::warn("BaseHttp::StartReadingProcess status code is 'Moved'.");
	  std::string moving_location;
	  std::getline(response_stream, moving_location);

	  if (moving_location.size() < 20) {
	    spdlog::info("BaseHttp::StartReadingProcess moved with no location.");
		return codes::HttpResultCodes::MovedWithNoLocation;
	  }

	  moving_location = std::move(moving_location.erase(0, 17));
	  moving_location = moving_location.substr(0, moving_location.size() - 2);

	  if (IsUrl(moving_location)) {
	    spdlog::info("BaseHttp::StartReadingProcess new location is '{}'.", moving_location);
		return Read(moving_location, app);
	  } else {
	    spdlog::info("BaseHttp::StartReadingProcess moved with no location.");
		return codes::HttpResultCodes::MovedWithNoLocation;
	  }
	}
    spdlog::trace("BaseHttp::StartReadingProcess status code is not 'Moved'.");

	if (status_code != codes::HttpStatusCodes::Success) {
	  spdlog::error("BaseHttp::StartReadingProcess got unrecognized error.");
	  LogBufferStream(response);
	  return codes::HttpResultCodes::ProcessingError;
	}
    spdlog::info("BaseHttp::StartReadingProcess status code is 'Success'.");

	// Read the response headers, which are terminated by a blank line.
	boost::asio::read_until(socket, response,
	                        fmt::format("{}{}", kRequestDelimiter, kRequestDelimiter));

	// Process the response headers.
	std::string header;
	while (std::getline(response_stream, header) && header != "\r") {
	  spdlog::debug("BaseHttp::StartReadingProcess printing response header...");
	  spdlog::debug("{}", header);
	  spdlog::debug("BaseHttp::StartReadingProcess done printing response header.");
	}
	std::cout << std::endl;

	// Write whatever content we already have to output.
	if (response.size() > 0) {
	  spdlog::debug("BaseHttp::StartReadingProcess printing response...");
	  std::cout << &response;
	  spdlog::debug("BaseHttp::StartReadingProcess done printing response.");
	}

	// Read until EOF, writing data to output as we go.
    spdlog::debug("BaseHttp::StartReadingProcess printing response until EOF...");
	while (boost::asio::read(socket, response,
	                         boost::asio::transfer_at_least(1), error)) {
	  std::cout << &response;
	}
    spdlog::debug("BaseHttp::StartReadingProcess done printing response.");

	if (error != nullptr) {
	  spdlog::error("BaseHttp::StartReadingProcess got {} error on request.", error);
	  return codes::HttpResultCodes::ProcessingError;
	}
    spdlog::info("BaseHttp::StartReadingProcess finished. Result is 'OK'.", error);
	return codes::HttpResultCodes::Ok;
  } catch (std::exception &e) {
    spdlog::error("BaseHttp::StartReadingProcess got unhandled exception. Message: '{}'.", e.what());
	return codes::HttpResultCodes::ProcessingException;
  }
}

void BaseHttp::LogBufferStream(buffer &buffer) {
  spdlog::trace("BaseHttp::LogBufferStream called.");
  size_t nBufferSize = boost::asio::buffer_size(buffer.data());

  // get const buffer
  std::stringstream ssOut;
  boost::asio::streambuf::const_buffers_type constBuffer = buffer.data();

  // copy const buffer to stringstream, then output
  std::copy(
	  boost::asio::buffers_begin(constBuffer),
	  boost::asio::buffers_begin(constBuffer) + nBufferSize,
	  std::ostream_iterator<char>(ssOut)
  );

  spdlog::debug("{}", ssOut.str());
}

tcp::socket BaseHttp::Connect(std::string_view url) const {
  spdlog::trace("BaseHttp::Connect called, url is '{}'.", url);
  boost::asio::io_service io_service;

  // Get a list of endpoints corresponding to the server name.
  tcp::resolver resolver(io_service);
  std::string &&protocol(GetProtocol().data());
  std::transform(protocol.begin(), protocol.end(), protocol.begin(), tolower);

  tcp::resolver::query query(url.data(), protocol);
  tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);

  // Try each endpoint until we successfully establish a connection.
  tcp::socket socket(io_service);
  boost::asio::connect(socket, endpoint_iterator);

  return socket;
}

void BaseHttp::GenerateRequest(buffer &request, std::string_view url, std::string_view app) const {
  spdlog::trace("BaseHttp::GenerateRequest called, url is '{}'; app is '{}'.", url, app);
  std::ostream request_stream(&request);

  if (app.empty()) {
	request_stream << GetRequestType() << " / " << GetSpecificProtocol() << kRequestDelimiter;
  } else {
	request_stream << GetRequestType() << " " << app << " " << GetSpecificProtocol() << kRequestDelimiter;
  }

  request_stream << "Host: " << url << kRequestDelimiter;
  request_stream << "Accept: */*" << kRequestDelimiter;
  request_stream << "Connection: close" << fmt::format("{}{}", kRequestDelimiter, kRequestDelimiter);
}

std::string BaseHttp::GetSpecificProtocol() const noexcept {
  return fmt::format("{}/{}", GetProtocol(), GetProtocolVersion());
}

bool BaseHttp::IsUrl(std::string_view url) noexcept {
  //boost::network::uri uri(url);
  //return std::regex_match (url, std::regex("^(https?:\/\/)?([\da-z\.-]+)\.([a-z\.]{2,6})([\/\w \.-]*)*\/?$"));
  return !url.empty();
}

}