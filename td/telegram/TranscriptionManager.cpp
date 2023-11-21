//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2023
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/TranscriptionManager.h"

#include "td/telegram/AuthManager.h"
#include "td/telegram/Global.h"
#include "td/telegram/logevent/LogEvent.h"
#include "td/telegram/Td.h"
#include "td/telegram/TdDb.h"

namespace td {

void TranscriptionManager::TrialParameters::update_left_tries() {
  if (cooldown_until_ <= G()->unix_time()) {
    cooldown_until_ = 0;
    left_tries_ = weekly_number_;
  } else if (left_tries_ > weekly_number_) {
    left_tries_ = weekly_number_;
  }
}

template <class StorerT>
void TranscriptionManager::TrialParameters::store(StorerT &storer) const {
  bool has_weekly_number = weekly_number_ != 0;
  bool has_duration_max = duration_max_ != 0;
  bool has_left_tries = left_tries_ != 0;
  bool has_cooldown_until = cooldown_until_ != 0;
  BEGIN_STORE_FLAGS();
  STORE_FLAG(has_weekly_number);
  STORE_FLAG(has_duration_max);
  STORE_FLAG(has_left_tries);
  STORE_FLAG(has_cooldown_until);
  END_STORE_FLAGS();
  if (has_weekly_number) {
    td::store(weekly_number_, storer);
  }
  if (has_duration_max) {
    td::store(duration_max_, storer);
  }
  if (has_left_tries) {
    td::store(left_tries_, storer);
  }
  if (has_cooldown_until) {
    td::store(cooldown_until_, storer);
  }
}

template <class ParserT>
void TranscriptionManager::TrialParameters::parse(ParserT &parser) {
  bool has_weekly_number;
  bool has_duration_max;
  bool has_left_tries;
  bool has_cooldown_until;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(has_weekly_number);
  PARSE_FLAG(has_duration_max);
  PARSE_FLAG(has_left_tries);
  PARSE_FLAG(has_cooldown_until);
  END_PARSE_FLAGS();
  if (has_weekly_number) {
    td::parse(weekly_number_, parser);
  }
  if (has_duration_max) {
    td::parse(duration_max_, parser);
  }
  if (has_left_tries) {
    td::parse(left_tries_, parser);
  }
  if (has_cooldown_until) {
    td::parse(cooldown_until_, parser);
  }
}

bool operator==(const TranscriptionManager::TrialParameters &lhs, const TranscriptionManager::TrialParameters &rhs) {
  return lhs.weekly_number_ == rhs.weekly_number_ && lhs.duration_max_ == rhs.duration_max_ &&
         lhs.left_tries_ == rhs.left_tries_ && lhs.cooldown_until_ == rhs.cooldown_until_;
}

TranscriptionManager::TranscriptionManager(Td *td, ActorShared<> parent) : td_(td), parent_(std::move(parent)) {
  load_trial_parameters();

  pending_audio_transcription_timeout_.set_callback(on_pending_audio_transcription_timeout_callback);
  pending_audio_transcription_timeout_.set_callback_data(static_cast<void *>(td_));
}

void TranscriptionManager::tear_down() {
  parent_.reset();
}

void TranscriptionManager::on_pending_audio_transcription_timeout_callback(void *td, int64 transcription_id) {
  if (G()->close_flag()) {
    return;
  }
  CHECK(td != nullptr);
  if (!static_cast<Td *>(td)->auth_manager_->is_authorized()) {
    return;
  }

  auto transcription_manager = static_cast<Td *>(td)->transcription_manager_.get();
  send_closure_later(transcription_manager->actor_id(transcription_manager),
                     &TranscriptionManager::on_pending_audio_transcription_failed, transcription_id,
                     Status::Error(500, "Timeout expired"));
}

string TranscriptionManager::get_trial_parameters_database_key() {
  return "speech_recognition_trial";
}

void TranscriptionManager::load_trial_parameters() {
  if (!td_->auth_manager_->is_authorized() || td_->auth_manager_->is_bot()) {
    return;
  }

  auto log_event_string = G()->td_db()->get_binlog_pmc()->get(get_trial_parameters_database_key());
  if (!log_event_string.empty()) {
    auto status = log_event_parse(trial_parameters_, log_event_string);
    if (status.is_ok()) {
      trial_parameters_.update_left_tries();
    } else {
      LOG(ERROR) << "Failed to parse speech recognition trial parameters from binlog: " << status;
      trial_parameters_ = TrialParameters();
      save_trial_parameters();
    }
  }
  send_update_speech_recognition_trial();
}

void TranscriptionManager::save_trial_parameters() {
  G()->td_db()->get_binlog_pmc()->set(get_trial_parameters_database_key(),
                                      log_event_store(trial_parameters_).as_slice().str());
}

void TranscriptionManager::on_update_trial_parameters(int32 weekly_number, int32 duration_max, int32 cooldown_until) {
  if (!td_->auth_manager_->is_authorized()) {
    return;
  }
  CHECK(!td_->auth_manager_->is_bot());
  TrialParameters new_trial_parameters;
  new_trial_parameters.weekly_number_ = max(0, weekly_number);
  new_trial_parameters.duration_max_ = max(0, duration_max);
  new_trial_parameters.cooldown_until_ = max(0, cooldown_until);
  new_trial_parameters.left_tries_ = trial_parameters_.left_tries_;
  new_trial_parameters.update_left_tries();
  if (new_trial_parameters == trial_parameters_) {
    return;
  }

  trial_parameters_ = std::move(new_trial_parameters);
  send_update_speech_recognition_trial();
  save_trial_parameters();
}

void TranscriptionManager::send_update_speech_recognition_trial() const {
  send_closure(G()->td(), &Td::send_update, get_update_speech_recognition_trial_object());
}

td_api::object_ptr<td_api::updateSpeechRecognitionTrial>
TranscriptionManager::get_update_speech_recognition_trial_object() const {
  CHECK(td_->auth_manager_->is_authorized());
  CHECK(!td_->auth_manager_->is_bot());
  return trial_parameters_.get_update_speech_recognition_trial_object();
}

td_api::object_ptr<td_api::updateSpeechRecognitionTrial>
TranscriptionManager::TrialParameters::get_update_speech_recognition_trial_object() const {
  return td_api::make_object<td_api::updateSpeechRecognitionTrial>(duration_max_, weekly_number_, left_tries_,
                                                                   cooldown_until_);
}

void TranscriptionManager::subscribe_to_transcribed_audio_updates(int64 transcription_id,
                                                                  TranscribedAudioHandler on_update) {
  CHECK(transcription_id != 0);
  if (pending_audio_transcriptions_.count(transcription_id) != 0) {
    on_pending_audio_transcription_failed(transcription_id,
                                          Status::Error(500, "Receive duplicate speech recognition identifier"));
  }
  bool is_inserted = pending_audio_transcriptions_.emplace(transcription_id, std::move(on_update)).second;
  CHECK(is_inserted);
  pending_audio_transcription_timeout_.set_timeout_in(transcription_id, AUDIO_TRANSCRIPTION_TIMEOUT);
}

void TranscriptionManager::on_update_transcribed_audio(
    telegram_api::object_ptr<telegram_api::updateTranscribedAudio> &&update) {
  auto it = pending_audio_transcriptions_.find(update->transcription_id_);
  if (it == pending_audio_transcriptions_.end()) {
    return;
  }
  // flags_, dialog_id_ and message_id_ must not be used
  if (!update->pending_) {
    auto on_update = std::move(it->second);
    pending_audio_transcriptions_.erase(it);
    pending_audio_transcription_timeout_.cancel_timeout(update->transcription_id_);
    on_update(std::move(update));
  } else {
    it->second(std::move(update));
  }
}

void TranscriptionManager::on_pending_audio_transcription_failed(int64 transcription_id, Status &&error) {
  if (G()->close_flag()) {
    return;
  }
  auto it = pending_audio_transcriptions_.find(transcription_id);
  if (it == pending_audio_transcriptions_.end()) {
    return;
  }
  auto on_update = std::move(it->second);
  pending_audio_transcriptions_.erase(it);
  pending_audio_transcription_timeout_.cancel_timeout(transcription_id);

  on_update(std::move(error));
}

void TranscriptionManager::get_current_state(vector<td_api::object_ptr<td_api::Update>> &updates) const {
  if (!td_->auth_manager_->is_authorized() || td_->auth_manager_->is_bot()) {
    return;
  }

  updates.push_back(get_update_speech_recognition_trial_object());
}

}  // namespace td