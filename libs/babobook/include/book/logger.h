//
// Created by Adminstudio on 6/25/2026.
//

#ifndef BABOMATCHINGENGINE_LOGGER_H
#define BABOMATCHINGENGINE_LOGGER_H

namespace babo::book {
/// @brief Interface to allow application to control error logging
class Logger
{
public:
  virtual void log_exception(const std::string & context, const std::exception& ex) = 0;
  virtual void log_message(const std::string & message) = 0;
};

}

#endif // BABOMATCHINGENGINE_LOGGER_H
