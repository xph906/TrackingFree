// GENERATED FILE
// This file generated by DomDistillerJs protoc plugin.
#include "third_party/dom_distiller_js/dom_distiller.pb.h"

// proto dependencies

// base dependencies
#include "base/values.h"
#include "base/memory/scoped_ptr.h"

#include <string>

namespace dom_distiller {
  namespace proto {
    namespace json {
      class DistilledContent {
       public:
        static dom_distiller::proto::DistilledContent ReadFromValue(const base::Value* json) {
          dom_distiller::proto::DistilledContent message;
          const base::DictionaryValue* dict;
          if (!json->GetAsDictionary(&dict)) goto error;
          if (dict->HasKey("1")) {
            std::string field_value;
            if (!dict->GetString("1", &field_value)) {
              goto error;
            }
            message.set_html(field_value);
          }
          return message;

        error:
          return dom_distiller::proto::DistilledContent();
        }

        static scoped_ptr<base::Value> WriteToValue(const dom_distiller::proto::DistilledContent& message) {
          scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
          if (message.has_html()) {
            dict->SetString("1", message.html());
          }
          return dict.PassAs<base::Value>();
        }
      };

      class PaginationInfo {
       public:
        static dom_distiller::proto::PaginationInfo ReadFromValue(const base::Value* json) {
          dom_distiller::proto::PaginationInfo message;
          const base::DictionaryValue* dict;
          if (!json->GetAsDictionary(&dict)) goto error;
          if (dict->HasKey("1")) {
            std::string field_value;
            if (!dict->GetString("1", &field_value)) {
              goto error;
            }
            message.set_next_page(field_value);
          }
          if (dict->HasKey("2")) {
            std::string field_value;
            if (!dict->GetString("2", &field_value)) {
              goto error;
            }
            message.set_prev_page(field_value);
          }
          if (dict->HasKey("3")) {
            std::string field_value;
            if (!dict->GetString("3", &field_value)) {
              goto error;
            }
            message.set_canonical_page(field_value);
          }
          return message;

        error:
          return dom_distiller::proto::PaginationInfo();
        }

        static scoped_ptr<base::Value> WriteToValue(const dom_distiller::proto::PaginationInfo& message) {
          scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
          if (message.has_next_page()) {
            dict->SetString("1", message.next_page());
          }
          if (message.has_prev_page()) {
            dict->SetString("2", message.prev_page());
          }
          if (message.has_canonical_page()) {
            dict->SetString("3", message.canonical_page());
          }
          return dict.PassAs<base::Value>();
        }
      };

      class DomDistillerResult {
       public:
        static dom_distiller::proto::DomDistillerResult ReadFromValue(const base::Value* json) {
          dom_distiller::proto::DomDistillerResult message;
          const base::DictionaryValue* dict;
          if (!json->GetAsDictionary(&dict)) goto error;
          if (dict->HasKey("1")) {
            std::string field_value;
            if (!dict->GetString("1", &field_value)) {
              goto error;
            }
            message.set_title(field_value);
          }
          if (dict->HasKey("2")) {
            const base::Value* inner_message_value;
            if (!dict->Get("2", &inner_message_value)) {
              goto error;
            }
            *message.mutable_distilled_content() =
                dom_distiller::proto::json::DistilledContent::ReadFromValue(inner_message_value);
          }
          if (dict->HasKey("3")) {
            const base::Value* inner_message_value;
            if (!dict->Get("3", &inner_message_value)) {
              goto error;
            }
            *message.mutable_pagination_info() =
                dom_distiller::proto::json::PaginationInfo::ReadFromValue(inner_message_value);
          }
          if (dict->HasKey("4")) {
            const base::ListValue* field_list;
            if (!dict->GetList("4", &field_list)) {
              goto error;
            }
            for (size_t i = 0; i < field_list->GetSize(); ++i) {
              std::string field_value;
              if (!field_list->GetString(i, &field_value)) {
                goto error;
              }
              message.add_image_urls(field_value);
            }
          }
          return message;

        error:
          return dom_distiller::proto::DomDistillerResult();
        }

        static scoped_ptr<base::Value> WriteToValue(const dom_distiller::proto::DomDistillerResult& message) {
          scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
          if (message.has_title()) {
            dict->SetString("1", message.title());
          }
          if (message.has_distilled_content()) {
            scoped_ptr<base::Value> inner_message_value =
                dom_distiller::proto::json::DistilledContent::WriteToValue(message.distilled_content());
            dict->Set("2", inner_message_value.release());
          }
          if (message.has_pagination_info()) {
            scoped_ptr<base::Value> inner_message_value =
                dom_distiller::proto::json::PaginationInfo::WriteToValue(message.pagination_info());
            dict->Set("3", inner_message_value.release());
          }
          base::ListValue* field_list = new base::ListValue();
          dict->Set("4", field_list);
          for (int i = 0; i < message.image_urls_size(); ++i) {
            field_list->AppendString(message.image_urls(i));
          }
          return dict.PassAs<base::Value>();
        }
      };

      class DomDistillerOptions {
       public:
        static dom_distiller::proto::DomDistillerOptions ReadFromValue(const base::Value* json) {
          dom_distiller::proto::DomDistillerOptions message;
          const base::DictionaryValue* dict;
          if (!json->GetAsDictionary(&dict)) goto error;
          if (dict->HasKey("1")) {
            bool field_value;
            if (!dict->GetBoolean("1", &field_value)) {
              goto error;
            }
            message.set_extract_text_only(field_value);
          }
          return message;

        error:
          return dom_distiller::proto::DomDistillerOptions();
        }

        static scoped_ptr<base::Value> WriteToValue(const dom_distiller::proto::DomDistillerOptions& message) {
          scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
          if (message.has_extract_text_only()) {
            dict->SetBoolean("1", message.extract_text_only());
          }
          return dict.PassAs<base::Value>();
        }
      };

    }
  }
}