%skeleton "lalr1.cc" // -*- C++ -*-
%require "3.5"  // fedora 32 has this one.

%defines  // %header starts from 3.8.1

%define api.namespace {dfly::json}
%define api.token.raw
%define api.token.constructor
%define api.value.type variant
%define api.parser.class {Parser}
%define parse.assert

// Added to header file before parser declaration.
%code requires {
  namespace dfly {
  namespace json {
    class Driver;
  }
  }
}

// Added to cc file
%code {

#include "src/core/json/lexer_impl.h"
#include "src/core/json/driver.h"

// Have to disable because GCC doesn't understand `symbol_type`'s union
// implementation
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

#define yylex driver->lexer()->Lex

using namespace std;
}

%parse-param { Driver *driver  }

%locations

%define parse.trace
%define parse.error verbose  // detailed
%define parse.lac full
%define api.token.prefix {TOK_}

%token
  LBRACKET "["
  RBRACKET "]"
  ROOT "$"
  DOT  "."
  WILDCARD "*"

// Needed 0 at the end to satisfy bison 3.5.1
%token YYEOF 0
%token <std::string> UNQ_STR "unquoted string"
%token <unsigned>    UINT "integer"

%%
// Based on the following specification:
// https://danielaparker.github.io/JsonCons.Net/articles/JsonPath/Specification.html

jsonpath: ROOT
        | ROOT relative_location

relative_location: DOT relative_path
        | LBRACKET bracket_expr RBRACKET

relative_path: identifier opt_relative_location
        | WILDCARD opt_relative_location

opt_relative_location:
        | relative_location

identifier: UNQ_STR
         // | single_quoted_string | double_quoted_string

bracket_expr: WILDCARD
            | index_expr

index_expr: UINT

%%


void dfly::json::Parser::error(const location_type& l, const string& m)
{
  cerr << l << ": " << m << '\n';
}