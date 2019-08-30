/*
 *  Copyright (c) 2018-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree.
 */

#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

#include <fizz/crypto/exchange/test/Mocks.h>
#include <fizz/protocol/clock/test/Mocks.h>
#include <fizz/protocol/test/Mocks.h>
#include <fizz/protocol/test/ProtocolTest.h>
#include <fizz/protocol/test/TestMessages.h>
#include <fizz/record/Extensions.h>
#include <fizz/record/test/Mocks.h>
#include <fizz/server/ServerProtocol.h>
#include <fizz/server/test/Mocks.h>
#include <fizz/util/Workarounds.h>
#include <folly/executors/ManualExecutor.h>

using namespace fizz::test;
using namespace folly;
using namespace testing;

namespace fizz {
namespace server {
namespace test {

class ServerProtocolTest : public ProtocolTest<ServerTypes, Actions> {
 public:
  void SetUp() override {
    context_ = std::make_shared<FizzServerContext>();
    context_->setSupportedVersions({ProtocolVersion::tls_1_3});
    auto mockFactory = std::make_unique<MockFactory>();
    mockFactory->setDefaults();
    factory_ = mockFactory.get();
    context_->setFactory(std::move(mockFactory));
    cert_ = std::make_shared<MockSelfCert>();
    auto certManager = std::make_unique<MockCertManager>();
    certManager_ = certManager.get();
    certVerifier_ = std::make_shared<MockCertificateVerifier>();
    context_->setClientCertVerifier(certVerifier_);
    context_->setCertManager(std::move(certManager));
    context_->setSupportedAlpns({"h2", "h3"});
    mockTicketCipher_ = std::make_shared<MockTicketCipher>();
    mockTicketCipher_->setDefaults(
        std::chrono::system_clock::time_point(std::chrono::seconds(10)));
    context_->setTicketCipher(mockTicketCipher_);
    extensions_ = std::make_shared<MockServerExtensions>();
    clock_ = std::make_shared<MockClock>();
    context_->setClock(clock_);

    ON_CALL(*certManager_, getCert(_, _, _))
        .WillByDefault(Return(CertManager::CertMatch(
            std::make_pair(cert_, SignatureScheme::ecdsa_secp256r1_sha256))));
    ON_CALL(*certManager_, getCert(_)).WillByDefault(Return(cert_));

    ON_CALL(*clock_, getCurrentTime())
        .WillByDefault(Return(
            std::chrono::system_clock::time_point(std::chrono::minutes(5))));

    replayCache_ = std::make_shared<MockReplayCache>();
    ON_CALL(*replayCache_, check(_)).WillByDefault(InvokeWithoutArgs([] {
      return folly::makeFuture(ReplayCacheResult::NotReplay);
    }));
  }

 protected:
  Actions getActions(AsyncActions asyncActions, bool immediate = true) {
    while (executor_.run())
      ;
    return folly::variant_match(
        asyncActions,
        ::fizz::detail::result_type<Actions>(),
        [immediate](folly::Future<Actions>& futureActions) {
          if (immediate) {
            EXPECT_TRUE(futureActions.hasValue());
          }
          return std::move(futureActions).get();
        },
        [](Actions& immediateActions) { return std::move(immediateActions); });
  }

  static std::unique_ptr<IOBuf> getEncryptedHandshakeWrite(
      EncryptedExtensions encryptedExt,
      CertificateMsg certificate,
      CertificateVerify verify,
      Finished finished) {
    auto buf = encodeHandshake(std::move(encryptedExt));
    buf->prependChain(encodeHandshake(std::move(certificate)));
    buf->prependChain(encodeHandshake(std::move(verify)));
    buf->prependChain(encodeHandshake(std::move(finished)));
    return buf;
  }

  static std::unique_ptr<IOBuf> getEncryptedHandshakeWrite(
      EncryptedExtensions encryptedExt,
      CertificateRequest request,
      CertificateMsg certificate,
      CertificateVerify verify,
      Finished finished) {
    auto buf = encodeHandshake(std::move(encryptedExt));
    buf->prependChain(encodeHandshake(std::move(request)));
    buf->prependChain(encodeHandshake(std::move(certificate)));
    buf->prependChain(encodeHandshake(std::move(verify)));
    buf->prependChain(encodeHandshake(std::move(finished)));
    return buf;
  }

  static std::unique_ptr<IOBuf> getEncryptedHandshakeWrite(
      EncryptedExtensions encryptedExt,
      CompressedCertificate certificate,
      CertificateVerify verify,
      Finished finished) {
    auto buf = encodeHandshake(std::move(encryptedExt));
    buf->prependChain(encodeHandshake(std::move(certificate)));
    buf->prependChain(encodeHandshake(std::move(verify)));
    buf->prependChain(encodeHandshake(std::move(finished)));
    return buf;
  }

  static std::unique_ptr<IOBuf> getEncryptedHandshakeWrite(
      EncryptedExtensions encryptedExt,
      Finished finished) {
    auto buf = encodeHandshake(std::move(encryptedExt));
    buf->prependChain(encodeHandshake(std::move(finished)));
    return buf;
  }

  void setMockRecord() {
    mockRead_ = new MockPlaintextReadRecordLayer();
    mockWrite_ = new MockPlaintextWriteRecordLayer();
    mockWrite_->setDefaults();
    state_.readRecordLayer().reset(mockRead_);
    state_.writeRecordLayer().reset(mockWrite_);
  }

  void setMockAppRecord() {
    appRead_ = new MockEncryptedReadRecordLayer(EncryptionLevel::AppTraffic);
    appWrite_ = new MockEncryptedWriteRecordLayer(EncryptionLevel::AppTraffic);
    appWrite_->setDefaults();
    state_.readRecordLayer().reset(appRead_);
    state_.writeRecordLayer().reset(appWrite_);
  }

  void setMockKeyScheduler() {
    mockKeyScheduler_ = new MockKeyScheduler();
    mockKeyScheduler_->setDefaults();
    state_.keyScheduler().reset(mockKeyScheduler_);
  }

  void setMockHandshakeContext() {
    mockHandshakeContext_ = new MockHandshakeContext();
    mockHandshakeContext_->setDefaults();
    state_.handshakeContext().reset(mockHandshakeContext_);
  }

  void acceptEarlyData() {
    context_->setEarlyDataSettings(
        true,
        {std::chrono::milliseconds(-1000000),
         std::chrono::milliseconds(1000000)},
        replayCache_);
  }

  void acceptCookies() {
    mockCookieCipher_ = std::make_shared<MockCookieCipher>();
    context_->setCookieCipher(mockCookieCipher_);
  }

  void expectCookie() {
    acceptCookies();
    EXPECT_CALL(*mockCookieCipher_, _decrypt(_))
        .WillOnce(Invoke([](Buf& cookie) {
          if (IOBufEqualTo()(cookie, IOBuf::copyBuffer("cookie"))) {
            CookieState cs;
            cs.version = TestProtocolVersion;
            cs.cipher = CipherSuite::TLS_AES_128_GCM_SHA256;
            cs.chloHash = IOBuf::copyBuffer("chlohash");
            cs.appToken = IOBuf::create(0);
            return folly::Optional<CookieState>(std::move(cs));
          } else {
            return folly::Optional<CookieState>();
          }
        }));
  }

  void setUpExpectingClientHello() {
    setMockRecord();
    state_.executor() = &executor_;
    state_.context() = context_;
    state_.state() = StateEnum::ExpectingClientHello;
    if (addExtensions_) {
      state_.extensions() = extensions_;
    }
  }

  void setUpExpectingClientHelloRetry() {
    setMockRecord();
    setMockHandshakeContext();
    state_.executor() = &executor_;
    state_.context() = context_;
    state_.version() = TestProtocolVersion;
    state_.cipher() = CipherSuite::TLS_AES_128_GCM_SHA256;
    state_.group() = NamedGroup::x25519;
    state_.keyExchangeType() = KeyExchangeType::HelloRetryRequest;
    state_.state() = StateEnum::ExpectingClientHello;
    if (addExtensions_) {
      state_.extensions() = extensions_;
    }
  }

  void setUpExpectingFinished() {
    setMockRecord();
    setMockKeyScheduler();
    setMockHandshakeContext();
    state_.executor() = &executor_;
    state_.context() = context_;
    state_.clientHandshakeSecret() = IOBuf::copyBuffer("clihandsec");
    state_.exporterMasterSecret() = IOBuf::copyBuffer("exportermaster");
    state_.version() = TestProtocolVersion;
    state_.cipher() = CipherSuite::TLS_AES_128_GCM_SHA256;
    state_.state() = StateEnum::ExpectingFinished;
    state_.serverCert() = cert_;
    state_.pskType() = PskType::NotAttempted;
    state_.alpn() = "h2";
    state_.handshakeTime() =
        std::chrono::system_clock::time_point(std::chrono::seconds(10));
  }

  void setUpExpectingCertificate() {
    setMockRecord();
    setMockKeyScheduler();
    setMockHandshakeContext();
    state_.executor() = &executor_;
    state_.context() = context_;
    state_.clientHandshakeSecret() = IOBuf::copyBuffer("clihandsec");
    state_.exporterMasterSecret() = IOBuf::copyBuffer("exportermaster");
    state_.version() = TestProtocolVersion;
    state_.cipher() = CipherSuite::TLS_AES_128_GCM_SHA256;
    state_.state() = StateEnum::ExpectingCertificate;
    state_.serverCert() = cert_;
    state_.pskType() = PskType::NotAttempted;
    state_.alpn() = "h2";
    context_->setClientAuthMode(ClientAuthMode::Required);
    state_.handshakeTime() =
        std::chrono::system_clock::time_point(std::chrono::seconds(10));
  }

  void setUpExpectingCertificateVerify() {
    setMockRecord();
    setMockKeyScheduler();
    setMockHandshakeContext();
    state_.executor() = &executor_;
    state_.context() = context_;
    state_.clientHandshakeSecret() = IOBuf::copyBuffer("clihandsec");
    state_.exporterMasterSecret() = IOBuf::copyBuffer("exportermaster");
    state_.version() = TestProtocolVersion;
    state_.cipher() = CipherSuite::TLS_AES_128_GCM_SHA256;
    state_.state() = StateEnum::ExpectingCertificateVerify;
    state_.serverCert() = cert_;
    clientIntCert_ = std::make_shared<MockPeerCert>();
    clientLeafCert_ = std::make_shared<MockPeerCert>();
    std::vector<std::shared_ptr<const PeerCert>> clientCerts = {clientLeafCert_,
                                                                clientIntCert_};
    state_.unverifiedCertChain() = std::move(clientCerts);
    state_.pskType() = PskType::NotAttempted;
    state_.alpn() = "h2";
    context_->setClientAuthMode(ClientAuthMode::Required);
    state_.handshakeTime() =
        std::chrono::system_clock::time_point(std::chrono::seconds(10));
  }

  void setUpAcceptingEarlyData() {
    setMockRecord();
    setMockKeyScheduler();
    setMockHandshakeContext();
    state_.handshakeReadRecordLayer().reset(
        new MockEncryptedReadRecordLayer(EncryptionLevel::Handshake));
    state_.version() = TestProtocolVersion;
    state_.cipher() = CipherSuite::TLS_AES_128_GCM_SHA256;
    state_.executor() = &executor_;
    state_.state() = StateEnum::AcceptingEarlyData;
    state_.context() = context_;
    state_.handshakeTime() =
        std::chrono::system_clock::time_point(std::chrono::seconds(10));
  }

  void setUpAcceptingData() {
    setMockAppRecord();
    setMockKeyScheduler();
    setMockHandshakeContext();
    state_.executor() = &executor_;
    state_.version() = TestProtocolVersion;
    state_.cipher() = CipherSuite::TLS_AES_128_GCM_SHA256;
    state_.context() = context_;
    state_.executor() = &executor_;
    state_.state() = StateEnum::AcceptingData;
    state_.serverCert() = cert_;
    state_.pskType() = PskType::NotAttempted;
    state_.handshakeTime() =
        std::chrono::system_clock::time_point(std::chrono::minutes(5));
    state_.alpn() = "h2";
  }

  ManualExecutor executor_;
  MockPlaintextReadRecordLayer* mockRead_;
  MockPlaintextWriteRecordLayer* mockWrite_;
  MockEncryptedReadRecordLayer* appRead_;
  MockEncryptedWriteRecordLayer* appWrite_;
  MockHandshakeContext* mockHandshakeContext_;
  MockKeyScheduler* mockKeyScheduler_;
  std::shared_ptr<MockTicketCipher> mockTicketCipher_;
  std::shared_ptr<MockCookieCipher> mockCookieCipher_;
  std::shared_ptr<FizzServerContext> context_;
  std::shared_ptr<MockSelfCert> cert_;
  std::shared_ptr<MockPeerCert> clientIntCert_;
  std::shared_ptr<MockPeerCert> clientLeafCert_;
  std::shared_ptr<MockCertificateVerifier> certVerifier_;
  MockCertManager* certManager_;
  std::shared_ptr<MockServerExtensions> extensions_;
  std::shared_ptr<MockReplayCache> replayCache_;
  std::shared_ptr<MockClock> clock_;
  bool addExtensions_ = true;
};

TEST_F(ServerProtocolTest, TestInvalidTransitionNoAlert) {
  auto actions =
      getActions(ServerStateMachine().processAppWrite(state_, AppWrite()));
  expectError<FizzException>(actions, none, "invalid event");
}

TEST_F(ServerProtocolTest, TestInvalidTransitionAlert) {
  setMockRecord();
  EXPECT_CALL(*mockWrite_, _write(_));
  auto actions =
      getActions(ServerStateMachine().processAppWrite(state_, AppWrite()));
  expectError<FizzException>(
      actions, AlertDescription::unexpected_message, "invalid event");
}

TEST_F(ServerProtocolTest, TestInvalidTransitionError) {
  state_.state() = StateEnum::Error;
  auto actions =
      getActions(ServerStateMachine().processAppWrite(state_, AppWrite()));
  EXPECT_TRUE(actions.empty());
}

TEST_F(ServerProtocolTest, TestAlertReceived) {
  setUpAcceptingData();
  Alert alert;
  alert.description = AlertDescription::unexpected_message;
  auto actions = getActions(detail::processEvent(state_, std::move(alert)));
  expectError<FizzException>(actions, folly::none, "received alert");
}

TEST_F(ServerProtocolTest, TestCloseNotifyReceived) {
  setUpAcceptingData();
  auto actions = getActions(detail::processEvent(state_, CloseNotify()));
  expectActions<MutateState, WriteToSocket, EndOfData>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::Closed);
  EXPECT_EQ(state_.readRecordLayer().get(), nullptr);
  EXPECT_EQ(state_.writeRecordLayer().get(), nullptr);
}

TEST_F(ServerProtocolTest, TestCloseNotifyReceivedWithUnparsedHandshakeData) {
  setUpAcceptingData();
  EXPECT_CALL(*appRead_, hasUnparsedHandshakeData())
      .WillRepeatedly(Return(true));
  auto actions = getActions(detail::processEvent(state_, CloseNotify()));
  expectError<FizzException>(actions, AlertDescription::unexpected_message);
}

TEST_F(ServerProtocolTest, TestAccept) {
  ReadRecordLayer* rrl;
  WriteRecordLayer* wrl;
  EXPECT_CALL(*factory_, makePlaintextReadRecordLayer())
      .WillOnce(Invoke([&rrl]() {
        auto ret = std::make_unique<PlaintextReadRecordLayer>();
        rrl = ret.get();
        return ret;
      }));
  EXPECT_CALL(*factory_, makePlaintextWriteRecordLayer())
      .WillOnce(Invoke([&wrl]() {
        auto ret = std::make_unique<PlaintextWriteRecordLayer>();
        wrl = ret.get();
        return ret;
      }));
  auto actions = getActions(ServerStateMachine().processAccept(
      state_, &executor_, context_, extensions_));
  expectActions<MutateState>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingClientHello);
  EXPECT_EQ(state_.executor(), &executor_);
  EXPECT_EQ(state_.context().get(), context_.get());
  EXPECT_EQ(state_.readRecordLayer().get(), rrl);
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::Plaintext);
  EXPECT_EQ(state_.writeRecordLayer().get(), wrl);
  EXPECT_EQ(
      state_.writeRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::Plaintext);
}

TEST_F(ServerProtocolTest, TestAppClose) {
  setUpAcceptingData();
  EXPECT_CALL(*appWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = appWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::alert);
    EXPECT_TRUE(IOBufEqualTo()(
        msg.fragment, encode(Alert(AlertDescription::close_notify))));
    content.data = IOBuf::copyBuffer("closenotify");
    return content;
  }));
  auto actions = ServerStateMachine().processAppClose(state_);
  expectActions<MutateState, WriteToSocket>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  EXPECT_EQ(write.contents[0].contentType, ContentType::alert);
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::AppTraffic);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingCloseNotify);
  EXPECT_NE(state_.readRecordLayer().get(), nullptr);
  EXPECT_EQ(state_.writeRecordLayer().get(), nullptr);

  EXPECT_CALL(*appRead_, mockReadEvent()).WillOnce(InvokeWithoutArgs([]() {
    Param p = CloseNotify(IOBuf::copyBuffer("ignoreddata"));
    return p;
  }));

  appRead_->useMockReadEvent(true);
  folly::IOBufQueue queue;
  auto actions2 = ServerStateMachine().processSocketData(state_, queue);
  folly::variant_match(
      actions2,
      [this](Actions& actions) {
        expectActions<MutateState, EndOfData>(actions);
        processStateMutations(actions);
        auto eod = expectAction<EndOfData>(actions);
        EXPECT_NE(eod.postTlsData, nullptr);
        auto expected = IOBuf::copyBuffer("ignoreddata");
        EXPECT_EQ(eod.postTlsData->coalesce(), expected->coalesce());
        EXPECT_EQ(state_.state(), StateEnum::Closed);
      },
      [](folly::Future<Actions>& futActions) { FAIL(); });
}

TEST_F(ServerProtocolTest, TestAppCloseNoWrite) {
  auto actions = ServerStateMachine().processAppClose(state_);
  expectActions<MutateState>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::Closed);
  EXPECT_EQ(state_.readRecordLayer().get(), nullptr);
  EXPECT_EQ(state_.writeRecordLayer().get(), nullptr);
}

TEST_F(ServerProtocolTest, TestAppCloseImmediate) {
  setUpAcceptingData();
  EXPECT_CALL(*appWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = appWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::alert);
    EXPECT_TRUE(IOBufEqualTo()(
        msg.fragment, encode(Alert(AlertDescription::close_notify))));
    content.data = IOBuf::copyBuffer("closenotify");
    return content;
  }));
  auto actions = ServerStateMachine().processAppCloseImmediate(state_);
  expectActions<MutateState, WriteToSocket>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  EXPECT_EQ(write.contents[0].contentType, ContentType::alert);
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::AppTraffic);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::Closed);
  EXPECT_EQ(state_.readRecordLayer().get(), nullptr);
  EXPECT_EQ(state_.writeRecordLayer().get(), nullptr);
}

TEST_F(ServerProtocolTest, TestAppCloseImmediateNoWrite) {
  auto actions = ServerStateMachine().processAppCloseImmediate(state_);
  expectActions<MutateState>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::Closed);
  EXPECT_EQ(state_.readRecordLayer().get(), nullptr);
  EXPECT_EQ(state_.writeRecordLayer().get(), nullptr);
}

TEST_F(ServerProtocolTest, TestClientHelloFullHandshakeFlow) {
  setUpExpectingClientHello();
  Sequence contextSeq;
  mockKeyScheduler_ = new MockKeyScheduler();
  mockHandshakeContext_ = new MockHandshakeContext();
  EXPECT_CALL(*factory_, makeKeyScheduler(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs(
          [=]() { return std::unique_ptr<KeyScheduler>(mockKeyScheduler_); }));
  EXPECT_CALL(
      *factory_, makeHandshakeContext(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs([=]() {
        return std::unique_ptr<HandshakeContext>(mockHandshakeContext_);
      }));
  EXPECT_CALL(
      *mockHandshakeContext_,
      appendToTranscript(BufMatches("clienthelloencoding")))
      .InSequence(contextSeq);
  EXPECT_CALL(*factory_, makeKeyExchange(NamedGroup::x25519))
      .WillOnce(InvokeWithoutArgs([]() {
        auto ret = std::make_unique<MockKeyExchange>();
        EXPECT_CALL(*ret, generateKeyPair());
        EXPECT_CALL(*ret, generateSharedSecret(RangeMatches("keyshare")))
            .WillOnce(InvokeWithoutArgs(
                []() { return IOBuf::copyBuffer("sharedsecret"); }));
        EXPECT_CALL(*ret, getKeyShare()).WillOnce(InvokeWithoutArgs([]() {
          return IOBuf::copyBuffer("servershare");
        }));
        return ret;
      }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(Invoke([]() { return IOBuf::copyBuffer("chlo_shlo"); }));
  EXPECT_CALL(
      *mockKeyScheduler_, deriveHandshakeSecret(RangeMatches("sharedsecret")));
  EXPECT_CALL(*factory_, makeRandom()).WillOnce(Invoke([]() {
    Random random;
    random.fill(0x44);
    return random;
  }));
  EXPECT_CALL(*mockWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = mockWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::handshake);
    EXPECT_TRUE(IOBufEqualTo()(
        msg.fragment, encodeHandshake(TestMessages::serverHello())));
    content.data = IOBuf::copyBuffer("writtenshlo");
    return content;
  }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ServerHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'h', 't'}),
            HandshakeSecrets::ServerHandshakeTraffic);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ClientHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'c', 'h', 't'}),
            HandshakeSecrets::ClientHandshakeTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverkey"),
                          IOBuf::copyBuffer("serveriv")};
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("cht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("clientkey"),
                          IOBuf::copyBuffer("clientiv")};
      }));
  EXPECT_CALL(*extensions_, getExtensions(_)).WillOnce(InvokeWithoutArgs([]() {
    Extension ext;
    ext.extension_type = ExtensionType::token_binding;
    ext.extension_data = folly::IOBuf::copyBuffer("someextension");
    std::vector<Extension> exts;
    exts.push_back(std::move(ext));
    return exts;
  }));

  MockAead* raead;
  MockAead* waead;
  MockAead* appwaead;
  MockEncryptedReadRecordLayer* rrl;
  MockEncryptedWriteRecordLayer* wrl;
  MockEncryptedWriteRecordLayer* appwrl;
  expectAeadCreation({{"clientkey", &raead},
                      {"serverkey", &waead},
                      {"serverappkey", &appwaead}});
  expectEncryptedReadRecordLayerCreation(
      &rrl, &raead, StringPiece("cht"), false);
  Sequence recSeq;
  expectEncryptedWriteRecordLayerCreation(
      &wrl,
      &waead,
      StringPiece("sht"),
      [](TLSMessage& msg, auto writeRecord) {
        EXPECT_EQ(msg.type, ContentType::handshake);

        auto modifiedEncryptedExt = TestMessages::encryptedExt();
        Extension ext;
        ext.extension_type = ExtensionType::token_binding;
        ext.extension_data = folly::IOBuf::copyBuffer("someextension");
        modifiedEncryptedExt.extensions.push_back(std::move(ext));
        EXPECT_TRUE(IOBufEqualTo()(
            msg.fragment,
            getEncryptedHandshakeWrite(
                std::move(modifiedEncryptedExt),
                TestMessages::certificate(),
                TestMessages::certificateVerify(),
                TestMessages::finished())));
        TLSContent content;
        content.contentType = msg.type;
        content.encryptionLevel = writeRecord->getEncryptionLevel();
        content.data = folly::IOBuf::copyBuffer("handshake");
        return content;
      },
      &recSeq);
  expectEncryptedWriteRecordLayerCreation(
      &appwrl, &appwaead, StringPiece("sat"), nullptr, &recSeq);
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*certManager_, getCert(_, _, _))
      .WillOnce(Invoke(
          [=](const folly::Optional<std::string>& sni,
              const std::vector<SignatureScheme>& /* supportedSigSchemes */,
              const std::vector<SignatureScheme>& peerSigSchemes) {
            EXPECT_EQ(*sni, "www.hostname.com");
            EXPECT_EQ(peerSigSchemes.size(), 2);
            EXPECT_EQ(
                peerSigSchemes[0], SignatureScheme::ecdsa_secp256r1_sha256);
            EXPECT_EQ(peerSigSchemes[1], SignatureScheme::rsa_pss_sha256);
            return CertManager::CertMatch(
                std::make_pair(cert_, SignatureScheme::ecdsa_secp256r1_sha256));
          }));
  EXPECT_CALL(*cert_, _getCertMessage(_));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(
          Invoke([]() { return IOBuf::copyBuffer("chlo_shlo_ee_cert"); }));
  EXPECT_CALL(
      *cert_,
      sign(
          SignatureScheme::ecdsa_secp256r1_sha256,
          CertificateVerifyContext::Server,
          RangeMatches("chlo_shlo_ee_cert")))
      .WillOnce(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("signature"); }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getFinishedData(RangeMatches("sht")))
      .InSequence(contextSeq)
      .WillRepeatedly(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(InvokeWithoutArgs(
          []() { return IOBuf::copyBuffer("chlo_shlo_ee_cert_sfin"); }));
  EXPECT_CALL(*mockKeyScheduler_, deriveMasterSecret());
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          MasterSecrets::ExporterMaster,
          RangeMatches("chlo_shlo_ee_cert_sfin")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'e', 'x', 'p', 'm'}),
            MasterSecrets::ExporterMaster);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      deriveAppTrafficSecrets(RangeMatches("chlo_shlo_ee_cert_sfin")));
  EXPECT_CALL(
      *mockKeyScheduler_, getSecret(AppTrafficSecrets::ServerAppTraffic))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'a', 't'}),
            AppTrafficSecrets::ServerAppTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sat"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverappkey"),
                          IOBuf::copyBuffer("serverappiv")};
      }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHello()));

  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  ASSERT_EQ(write.contents.size(), 2);

  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::Plaintext);
  EXPECT_EQ(write.contents[0].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[0].data, IOBuf::copyBuffer("writtenshlo")));

  EXPECT_EQ(write.contents[1].encryptionLevel, EncryptionLevel::Handshake);
  EXPECT_EQ(write.contents[1].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[1].data, IOBuf::copyBuffer("handshake")));

  expectSecret(
      actions, HandshakeSecrets::ClientHandshakeTraffic, StringPiece("cht"));
  expectSecret(
      actions, HandshakeSecrets::ServerHandshakeTraffic, StringPiece("sht"));
  expectSecret(
      actions, AppTrafficSecrets::ServerAppTraffic, StringPiece("sat"));
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.readRecordLayer().get(), rrl);
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::Handshake);
  EXPECT_EQ(state_.writeRecordLayer().get(), appwrl);
  EXPECT_EQ(
      state_.writeRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::AppTraffic);
  EXPECT_EQ(state_.handshakeContext().get(), mockHandshakeContext_);
  EXPECT_EQ(state_.keyScheduler().get(), mockKeyScheduler_);
  EXPECT_EQ(state_.serverCert(), cert_);
  EXPECT_EQ(state_.version(), TestProtocolVersion);
  EXPECT_EQ(state_.cipher(), CipherSuite::TLS_AES_128_GCM_SHA256);
  EXPECT_EQ(state_.group(), NamedGroup::x25519);
  EXPECT_EQ(state_.sigScheme(), SignatureScheme::ecdsa_secp256r1_sha256);
  EXPECT_EQ(state_.pskType(), PskType::NotAttempted);
  EXPECT_FALSE(state_.pskMode().hasValue());
  EXPECT_EQ(state_.keyExchangeType(), KeyExchangeType::OneRtt);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::NotAttempted);
  EXPECT_EQ(state_.replayCacheResult(), ReplayCacheResult::NotChecked);
  EXPECT_FALSE(state_.clientClockSkew().hasValue());
  EXPECT_EQ(*state_.alpn(), "h2");
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.clientHandshakeSecret(), IOBuf::copyBuffer("cht")));
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.exporterMasterSecret(), IOBuf::copyBuffer("expm")));
  EXPECT_FALSE(state_.earlyExporterMasterSecret().hasValue());
}

TEST_F(ServerProtocolTest, TestClientHelloCompressedCertFlow) {
  setUpExpectingClientHello();
  Sequence contextSeq;
  mockKeyScheduler_ = new MockKeyScheduler();
  mockHandshakeContext_ = new MockHandshakeContext();
  EXPECT_CALL(*factory_, makeKeyScheduler(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs(
          [=]() { return std::unique_ptr<KeyScheduler>(mockKeyScheduler_); }));
  EXPECT_CALL(
      *factory_, makeHandshakeContext(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs([=]() {
        return std::unique_ptr<HandshakeContext>(mockHandshakeContext_);
      }));
  EXPECT_CALL(
      *mockHandshakeContext_,
      appendToTranscript(BufMatches("clienthelloencoding")))
      .InSequence(contextSeq);
  EXPECT_CALL(*factory_, makeKeyExchange(NamedGroup::x25519))
      .WillOnce(InvokeWithoutArgs([]() {
        auto ret = std::make_unique<MockKeyExchange>();
        EXPECT_CALL(*ret, generateKeyPair());
        EXPECT_CALL(*ret, generateSharedSecret(RangeMatches("keyshare")))
            .WillOnce(InvokeWithoutArgs(
                []() { return IOBuf::copyBuffer("sharedsecret"); }));
        EXPECT_CALL(*ret, getKeyShare()).WillOnce(InvokeWithoutArgs([]() {
          return IOBuf::copyBuffer("servershare");
        }));
        return ret;
      }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(Invoke([]() { return IOBuf::copyBuffer("chlo_shlo"); }));
  EXPECT_CALL(
      *mockKeyScheduler_, deriveHandshakeSecret(RangeMatches("sharedsecret")));
  EXPECT_CALL(*factory_, makeRandom()).WillOnce(Invoke([]() {
    Random random;
    random.fill(0x44);
    return random;
  }));
  EXPECT_CALL(*mockWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = mockWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::handshake);
    EXPECT_TRUE(IOBufEqualTo()(
        msg.fragment, encodeHandshake(TestMessages::serverHello())));
    content.data = IOBuf::copyBuffer("writtenshlo");
    return content;
  }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ServerHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'h', 't'}),
            HandshakeSecrets::ServerHandshakeTraffic);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ClientHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'c', 'h', 't'}),
            HandshakeSecrets::ClientHandshakeTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverkey"),
                          IOBuf::copyBuffer("serveriv")};
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("cht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("clientkey"),
                          IOBuf::copyBuffer("clientiv")};
      }));
  EXPECT_CALL(*extensions_, getExtensions(_)).WillOnce(InvokeWithoutArgs([]() {
    Extension ext;
    ext.extension_type = ExtensionType::token_binding;
    ext.extension_data = folly::IOBuf::copyBuffer("someextension");
    std::vector<Extension> exts;
    exts.push_back(std::move(ext));
    return exts;
  }));

  MockAead* raead;
  MockAead* waead;
  MockAead* appwaead;
  MockEncryptedReadRecordLayer* rrl;
  MockEncryptedWriteRecordLayer* wrl;
  MockEncryptedWriteRecordLayer* appwrl;
  expectAeadCreation({{"clientkey", &raead},
                      {"serverkey", &waead},
                      {"serverappkey", &appwaead}});
  expectEncryptedReadRecordLayerCreation(
      &rrl, &raead, StringPiece("cht"), false);
  Sequence recSeq;
  expectEncryptedWriteRecordLayerCreation(
      &wrl,
      &waead,
      StringPiece("sht"),
      [](TLSMessage& msg, auto writeRecord) {
        EXPECT_EQ(msg.type, ContentType::handshake);

        auto modifiedEncryptedExt = TestMessages::encryptedExt();
        Extension ext;
        ext.extension_type = ExtensionType::token_binding;
        ext.extension_data = folly::IOBuf::copyBuffer("someextension");
        modifiedEncryptedExt.extensions.push_back(std::move(ext));
        EXPECT_TRUE(IOBufEqualTo()(
            msg.fragment,
            getEncryptedHandshakeWrite(
                std::move(modifiedEncryptedExt),
                TestMessages::compressedCertificate(),
                TestMessages::certificateVerify(),
                TestMessages::finished())));
        TLSContent content;
        content.contentType = msg.type;
        content.encryptionLevel = writeRecord->getEncryptionLevel();
        content.data = folly::IOBuf::copyBuffer("handshake");
        return content;
      },
      &recSeq);
  expectEncryptedWriteRecordLayerCreation(
      &appwrl, &appwaead, StringPiece("sat"), nullptr, &recSeq);
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*certManager_, getCert(_, _, _))
      .WillOnce(Invoke(
          [=](const folly::Optional<std::string>& sni,
              const std::vector<SignatureScheme>& /* supportedSigSchemes */,
              const std::vector<SignatureScheme>& peerSigSchemes) {
            EXPECT_EQ(*sni, "www.hostname.com");
            EXPECT_EQ(peerSigSchemes.size(), 2);
            EXPECT_EQ(
                peerSigSchemes[0], SignatureScheme::ecdsa_secp256r1_sha256);
            EXPECT_EQ(peerSigSchemes[1], SignatureScheme::rsa_pss_sha256);
            return CertManager::CertMatch(
                std::make_pair(cert_, SignatureScheme::ecdsa_secp256r1_sha256));
          }));
  context_->setSupportedCompressionAlgorithms(
      {CertificateCompressionAlgorithm::zlib});
  EXPECT_CALL(*cert_, getCompressedCert(_)).WillOnce(InvokeWithoutArgs([]() {
    return TestMessages::compressedCertificate();
  }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(
          Invoke([]() { return IOBuf::copyBuffer("chlo_shlo_ee_compcert"); }));
  EXPECT_CALL(
      *cert_,
      sign(
          SignatureScheme::ecdsa_secp256r1_sha256,
          CertificateVerifyContext::Server,
          RangeMatches("chlo_shlo_ee_compcert")))
      .WillOnce(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("signature"); }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getFinishedData(RangeMatches("sht")))
      .InSequence(contextSeq)
      .WillRepeatedly(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(InvokeWithoutArgs(
          []() { return IOBuf::copyBuffer("chlo_shlo_ee_compcert_sfin"); }));
  EXPECT_CALL(*mockKeyScheduler_, deriveMasterSecret());
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          MasterSecrets::ExporterMaster,
          RangeMatches("chlo_shlo_ee_compcert_sfin")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'e', 'x', 'p', 'm'}),
            MasterSecrets::ExporterMaster);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      deriveAppTrafficSecrets(RangeMatches("chlo_shlo_ee_compcert_sfin")));
  EXPECT_CALL(
      *mockKeyScheduler_, getSecret(AppTrafficSecrets::ServerAppTraffic))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'a', 't'}),
            AppTrafficSecrets::ServerAppTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sat"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverappkey"),
                          IOBuf::copyBuffer("serverappiv")};
      }));

  auto chlo = TestMessages::clientHello();
  CertificateCompressionAlgorithms algos;
  algos.algorithms = {CertificateCompressionAlgorithm::zlib};
  chlo.extensions.push_back(encodeExtension(algos));
  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));

  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  ASSERT_EQ(write.contents.size(), 2);
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::Plaintext);
  EXPECT_EQ(write.contents[0].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[0].data, IOBuf::copyBuffer("writtenshlo")));

  EXPECT_EQ(write.contents[1].encryptionLevel, EncryptionLevel::Handshake);
  EXPECT_EQ(write.contents[1].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[1].data, IOBuf::copyBuffer("handshake")));

  expectSecret(
      actions, HandshakeSecrets::ClientHandshakeTraffic, StringPiece("cht"));
  expectSecret(
      actions, HandshakeSecrets::ServerHandshakeTraffic, StringPiece("sht"));
  expectSecret(
      actions, AppTrafficSecrets::ServerAppTraffic, StringPiece("sat"));
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.readRecordLayer().get(), rrl);
  EXPECT_EQ(state_.writeRecordLayer().get(), appwrl);
  EXPECT_EQ(state_.handshakeContext().get(), mockHandshakeContext_);
  EXPECT_EQ(state_.keyScheduler().get(), mockKeyScheduler_);
  EXPECT_EQ(state_.serverCert(), cert_);
  EXPECT_EQ(state_.serverCertCompAlgo(), CertificateCompressionAlgorithm::zlib);
  EXPECT_EQ(state_.version(), TestProtocolVersion);
  EXPECT_EQ(state_.cipher(), CipherSuite::TLS_AES_128_GCM_SHA256);
  EXPECT_EQ(state_.group(), NamedGroup::x25519);
  EXPECT_EQ(state_.sigScheme(), SignatureScheme::ecdsa_secp256r1_sha256);
  EXPECT_EQ(state_.pskType(), PskType::NotAttempted);
  EXPECT_FALSE(state_.pskMode().hasValue());
  EXPECT_EQ(state_.keyExchangeType(), KeyExchangeType::OneRtt);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::NotAttempted);
  EXPECT_EQ(state_.replayCacheResult(), ReplayCacheResult::NotChecked);
  EXPECT_FALSE(state_.clientClockSkew().hasValue());
  EXPECT_EQ(*state_.alpn(), "h2");
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.clientHandshakeSecret(), IOBuf::copyBuffer("cht")));
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.exporterMasterSecret(), IOBuf::copyBuffer("expm")));
  EXPECT_FALSE(state_.earlyExporterMasterSecret().hasValue());
}

TEST_F(ServerProtocolTest, TestClientHelloCertRequestFlow) {
  setUpExpectingClientHello();
  context_->setClientAuthMode(ClientAuthMode::Required);
  Sequence contextSeq;
  mockKeyScheduler_ = new MockKeyScheduler();
  mockHandshakeContext_ = new MockHandshakeContext();
  EXPECT_CALL(*factory_, makeKeyScheduler(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs(
          [=]() { return std::unique_ptr<KeyScheduler>(mockKeyScheduler_); }));
  EXPECT_CALL(
      *factory_, makeHandshakeContext(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs([=]() {
        return std::unique_ptr<HandshakeContext>(mockHandshakeContext_);
      }));
  EXPECT_CALL(
      *mockHandshakeContext_,
      appendToTranscript(BufMatches("clienthelloencoding")))
      .InSequence(contextSeq);
  EXPECT_CALL(*factory_, makeKeyExchange(NamedGroup::x25519))
      .WillOnce(InvokeWithoutArgs([]() {
        auto ret = std::make_unique<MockKeyExchange>();
        EXPECT_CALL(*ret, generateKeyPair());
        EXPECT_CALL(*ret, generateSharedSecret(RangeMatches("keyshare")))
            .WillOnce(InvokeWithoutArgs(
                []() { return IOBuf::copyBuffer("sharedsecret"); }));
        EXPECT_CALL(*ret, getKeyShare()).WillOnce(InvokeWithoutArgs([]() {
          return IOBuf::copyBuffer("servershare");
        }));
        return ret;
      }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(Invoke([]() { return IOBuf::copyBuffer("chlo_shlo"); }));
  EXPECT_CALL(
      *mockKeyScheduler_, deriveHandshakeSecret(RangeMatches("sharedsecret")));
  EXPECT_CALL(*factory_, makeRandom()).WillOnce(Invoke([]() {
    Random random;
    random.fill(0x44);
    return random;
  }));
  EXPECT_CALL(*mockWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = mockWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::handshake);
    EXPECT_TRUE(IOBufEqualTo()(
        msg.fragment, encodeHandshake(TestMessages::serverHello())));
    content.data = IOBuf::copyBuffer("writtenshlo");
    return content;
  }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ServerHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'h', 't'}),
            HandshakeSecrets::ServerHandshakeTraffic);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ClientHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'c', 'h', 't'}),
            HandshakeSecrets::ClientHandshakeTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverkey"),
                          IOBuf::copyBuffer("serveriv")};
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("cht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("clientkey"),
                          IOBuf::copyBuffer("clientiv")};
      }));
  EXPECT_CALL(*extensions_, getExtensions(_)).WillOnce(InvokeWithoutArgs([]() {
    Extension ext;
    ext.extension_type = ExtensionType::token_binding;
    ext.extension_data = folly::IOBuf::copyBuffer("someextension");
    std::vector<Extension> exts;
    exts.push_back(std::move(ext));
    return exts;
  }));

  MockAead* raead;
  MockAead* waead;
  MockAead* appwaead;
  MockEncryptedReadRecordLayer* rrl;
  MockEncryptedWriteRecordLayer* wrl;
  MockEncryptedWriteRecordLayer* appwrl;
  expectAeadCreation({{"clientkey", &raead},
                      {"serverkey", &waead},
                      {"serverappkey", &appwaead}});
  expectEncryptedReadRecordLayerCreation(
      &rrl, &raead, StringPiece("cht"), false);
  Sequence recSeq;
  expectEncryptedWriteRecordLayerCreation(
      &wrl,
      &waead,
      StringPiece("sht"),
      [](TLSMessage& msg, auto writeRecord) {
        EXPECT_EQ(msg.type, ContentType::handshake);
        auto modifiedEncryptedExt = TestMessages::encryptedExt();
        Extension ext;
        ext.extension_type = ExtensionType::token_binding;
        ext.extension_data = folly::IOBuf::copyBuffer("someextension");
        modifiedEncryptedExt.extensions.push_back(std::move(ext));
        EXPECT_TRUE(IOBufEqualTo()(
            msg.fragment,
            getEncryptedHandshakeWrite(
                std::move(modifiedEncryptedExt),
                TestMessages::certificateRequest(),
                TestMessages::certificate(),
                TestMessages::certificateVerify(),
                TestMessages::finished())));
        TLSContent content;
        content.contentType = msg.type;
        content.encryptionLevel = writeRecord->getEncryptionLevel();
        content.data = folly::IOBuf::copyBuffer("handshake");
        return content;
      },
      &recSeq);
  expectEncryptedWriteRecordLayerCreation(
      &appwrl, &appwaead, StringPiece("sat"), nullptr, &recSeq);
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*certManager_, getCert(_, _, _))
      .WillOnce(Invoke(
          [=](const folly::Optional<std::string>& sni,
              const std::vector<SignatureScheme>& /* supportedSigSchemes */,
              const std::vector<SignatureScheme>& peerSigSchemes) {
            EXPECT_EQ(*sni, "www.hostname.com");
            EXPECT_EQ(peerSigSchemes.size(), 2);
            EXPECT_EQ(
                peerSigSchemes[0], SignatureScheme::ecdsa_secp256r1_sha256);
            EXPECT_EQ(peerSigSchemes[1], SignatureScheme::rsa_pss_sha256);
            return CertManager::CertMatch(
                std::make_pair(cert_, SignatureScheme::ecdsa_secp256r1_sha256));
          }));
  EXPECT_CALL(*cert_, _getCertMessage(_));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(
          Invoke([]() { return IOBuf::copyBuffer("chlo_shlo_ee_cert"); }));
  EXPECT_CALL(
      *cert_,
      sign(
          SignatureScheme::ecdsa_secp256r1_sha256,
          CertificateVerifyContext::Server,
          RangeMatches("chlo_shlo_ee_cert")))
      .WillOnce(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("signature"); }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getFinishedData(RangeMatches("sht")))
      .InSequence(contextSeq)
      .WillRepeatedly(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(InvokeWithoutArgs(
          []() { return IOBuf::copyBuffer("chlo_shlo_ee_cert_sfin"); }));
  EXPECT_CALL(*mockKeyScheduler_, deriveMasterSecret());
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          MasterSecrets::ExporterMaster,
          RangeMatches("chlo_shlo_ee_cert_sfin")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'e', 'x', 'p', 'm'}),
            MasterSecrets::ExporterMaster);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      deriveAppTrafficSecrets(RangeMatches("chlo_shlo_ee_cert_sfin")));
  EXPECT_CALL(
      *mockKeyScheduler_, getSecret(AppTrafficSecrets::ServerAppTraffic))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'a', 't'}),
            AppTrafficSecrets::ServerAppTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sat"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverappkey"),
                          IOBuf::copyBuffer("serverappiv")};
      }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHello()));

  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  ASSERT_EQ(write.contents.size(), 2);
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::Plaintext);
  EXPECT_EQ(write.contents[0].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[0].data, IOBuf::copyBuffer("writtenshlo")));

  EXPECT_EQ(write.contents[1].encryptionLevel, EncryptionLevel::Handshake);
  EXPECT_EQ(write.contents[1].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[1].data, IOBuf::copyBuffer("handshake")));
  expectSecret(
      actions, HandshakeSecrets::ClientHandshakeTraffic, StringPiece("cht"));
  expectSecret(
      actions, HandshakeSecrets::ServerHandshakeTraffic, StringPiece("sht"));
  expectSecret(
      actions, AppTrafficSecrets::ServerAppTraffic, StringPiece("sat"));
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingCertificate);
  EXPECT_EQ(state_.readRecordLayer().get(), rrl);
  EXPECT_EQ(state_.writeRecordLayer().get(), appwrl);
  EXPECT_EQ(state_.handshakeContext().get(), mockHandshakeContext_);
  EXPECT_EQ(state_.keyScheduler().get(), mockKeyScheduler_);
  EXPECT_EQ(state_.serverCert(), cert_);
  EXPECT_EQ(state_.version(), TestProtocolVersion);
  EXPECT_EQ(state_.cipher(), CipherSuite::TLS_AES_128_GCM_SHA256);
  EXPECT_EQ(state_.group(), NamedGroup::x25519);
  EXPECT_EQ(state_.sigScheme(), SignatureScheme::ecdsa_secp256r1_sha256);
  EXPECT_EQ(state_.pskType(), PskType::NotAttempted);
  EXPECT_FALSE(state_.pskMode().hasValue());
  EXPECT_EQ(state_.keyExchangeType(), KeyExchangeType::OneRtt);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::NotAttempted);
  EXPECT_EQ(state_.replayCacheResult(), ReplayCacheResult::NotChecked);
  EXPECT_FALSE(state_.clientClockSkew().hasValue());
  EXPECT_EQ(*state_.alpn(), "h2");
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.clientHandshakeSecret(), IOBuf::copyBuffer("cht")));
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.exporterMasterSecret(), IOBuf::copyBuffer("expm")));
  EXPECT_FALSE(state_.earlyExporterMasterSecret().hasValue());
}

TEST_F(ServerProtocolTest, TestClientHelloPskFlow) {
  context_->setSupportedPskModes({PskKeyExchangeMode::psk_ke});
  setUpExpectingClientHello();
  EXPECT_CALL(*mockTicketCipher_, _decrypt(_))
      .WillOnce(InvokeWithoutArgs([=]() {
        ResumptionState res;
        res.version = TestProtocolVersion;
        res.cipher = CipherSuite::TLS_AES_128_GCM_SHA256;
        res.resumptionSecret = folly::IOBuf::copyBuffer("resumesecret");
        res.serverCert = cert_;
        res.handshakeTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(1));
        return std::make_pair(PskType::Resumption, std::move(res));
      }));
  Sequence contextSeq;
  mockKeyScheduler_ = new MockKeyScheduler();
  mockHandshakeContext_ = new MockHandshakeContext();
  EXPECT_CALL(*factory_, makeKeyScheduler(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs(
          [=]() { return std::unique_ptr<KeyScheduler>(mockKeyScheduler_); }));
  EXPECT_CALL(
      *factory_, makeHandshakeContext(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs([=]() {
        return std::unique_ptr<HandshakeContext>(mockHandshakeContext_);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_, deriveEarlySecret(RangeMatches("resumesecret")));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(BufMatches("client")))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getFinishedData(RangeMatches("bdr")))
      .InSequence(contextSeq)
      .WillRepeatedly(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(
      *mockHandshakeContext_, appendToTranscript(BufMatches("helloencoding")))
      .InSequence(contextSeq);
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(EarlySecrets::ResumptionPskBinder, RangeMatches("")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'b', 'd', 'r'}),
            EarlySecrets::ResumptionPskBinder);
      }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(Invoke([]() { return IOBuf::copyBuffer("chlo_shlo"); }));
  EXPECT_CALL(*mockKeyScheduler_, deriveHandshakeSecret());
  EXPECT_CALL(*factory_, makeRandom()).WillOnce(Invoke([]() {
    Random random;
    random.fill(0x44);
    return random;
  }));
  EXPECT_CALL(*mockWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = mockWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::handshake);
    auto shlo = TestMessages::serverHello();
    TestMessages::removeExtension(shlo, ExtensionType::key_share);
    ServerPresharedKey serverPsk;
    serverPsk.selected_identity = 0;
    shlo.extensions.push_back(encodeExtension(std::move(serverPsk)));
    EXPECT_TRUE(IOBufEqualTo()(msg.fragment, encodeHandshake(std::move(shlo))));
    content.data = IOBuf::copyBuffer("writtenshlo");
    return content;
  }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ServerHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'h', 't'}),
            HandshakeSecrets::ServerHandshakeTraffic);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ClientHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'c', 'h', 't'}),
            HandshakeSecrets::ClientHandshakeTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverkey"),
                          IOBuf::copyBuffer("serveriv")};
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("cht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("clientkey"),
                          IOBuf::copyBuffer("clientiv")};
      }));
  MockAead* raead;
  MockAead* waead;
  MockAead* appwaead;
  MockEncryptedReadRecordLayer* rrl;
  MockEncryptedWriteRecordLayer* wrl;
  MockEncryptedWriteRecordLayer* appwrl;
  expectAeadCreation({{"clientkey", &raead},
                      {"serverkey", &waead},
                      {"serverappkey", &appwaead}});
  expectEncryptedReadRecordLayerCreation(
      &rrl, &raead, StringPiece("cht"), false);
  Sequence recSeq;
  expectEncryptedWriteRecordLayerCreation(
      &wrl,
      &waead,
      StringPiece("sht"),
      [](TLSMessage& msg, auto writeRecord) {
        EXPECT_EQ(msg.type, ContentType::handshake);
        EXPECT_TRUE(IOBufEqualTo()(
            msg.fragment,
            getEncryptedHandshakeWrite(
                TestMessages::encryptedExt(), TestMessages::finished())));
        TLSContent content;
        content.contentType = msg.type;
        content.encryptionLevel = writeRecord->getEncryptionLevel();
        content.data = folly::IOBuf::copyBuffer("handshake");
        return content;
      },
      &recSeq);
  expectEncryptedWriteRecordLayerCreation(
      &appwrl, &appwaead, StringPiece("sat"), nullptr, &recSeq);
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getFinishedData(RangeMatches("sht")))
      .InSequence(contextSeq)
      .WillRepeatedly(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(InvokeWithoutArgs(
          []() { return IOBuf::copyBuffer("chlo_shlo_sfin"); }));
  EXPECT_CALL(*mockKeyScheduler_, deriveMasterSecret());
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(MasterSecrets::ExporterMaster, RangeMatches("chlo_shlo_sfin")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'e', 'x', 'p', 'm'}),
            MasterSecrets::ExporterMaster);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      deriveAppTrafficSecrets(RangeMatches("chlo_shlo_sfin")));
  EXPECT_CALL(
      *mockKeyScheduler_, getSecret(AppTrafficSecrets::ServerAppTraffic))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'a', 't'}),
            AppTrafficSecrets::ServerAppTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sat"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverappkey"),
                          IOBuf::copyBuffer("serverappiv")};
      }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHelloPsk()));

  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  ASSERT_EQ(write.contents.size(), 2);
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::Plaintext);
  EXPECT_EQ(write.contents[0].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[0].data, IOBuf::copyBuffer("writtenshlo")));
  EXPECT_EQ(write.contents[1].encryptionLevel, EncryptionLevel::Handshake);
  EXPECT_EQ(write.contents[1].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[1].data, IOBuf::copyBuffer("handshake")));

  expectSecret(
      actions, HandshakeSecrets::ClientHandshakeTraffic, StringPiece("cht"));
  expectSecret(
      actions, HandshakeSecrets::ServerHandshakeTraffic, StringPiece("sht"));
  expectSecret(
      actions, AppTrafficSecrets::ServerAppTraffic, StringPiece("sat"));
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.readRecordLayer().get(), rrl);
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::Handshake);
  EXPECT_EQ(state_.writeRecordLayer().get(), appwrl);
  EXPECT_EQ(
      state_.writeRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::AppTraffic);
  EXPECT_EQ(state_.handshakeContext().get(), mockHandshakeContext_);
  EXPECT_EQ(state_.keyScheduler().get(), mockKeyScheduler_);
  EXPECT_EQ(state_.serverCert(), cert_);
  EXPECT_EQ(state_.version(), TestProtocolVersion);
  EXPECT_EQ(state_.cipher(), CipherSuite::TLS_AES_128_GCM_SHA256);
  EXPECT_FALSE(state_.group().hasValue());
  EXPECT_FALSE(state_.sigScheme().hasValue());
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.pskMode(), PskKeyExchangeMode::psk_ke);
  EXPECT_EQ(state_.keyExchangeType(), KeyExchangeType::None);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::NotAttempted);
  EXPECT_EQ(state_.replayCacheResult(), ReplayCacheResult::NotChecked);
  EXPECT_TRUE(state_.clientClockSkew().hasValue());
  EXPECT_EQ(*state_.alpn(), "h2");
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.clientHandshakeSecret(), IOBuf::copyBuffer("cht")));
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.exporterMasterSecret(), IOBuf::copyBuffer("expm")));
  EXPECT_FALSE(state_.earlyExporterMasterSecret().hasValue());
}

TEST_F(ServerProtocolTest, TestClientHelloPskDheFlow) {
  context_->setSupportedPskModes({PskKeyExchangeMode::psk_dhe_ke});
  setUpExpectingClientHello();
  EXPECT_CALL(*mockTicketCipher_, _decrypt(_))
      .WillOnce(InvokeWithoutArgs([=]() {
        ResumptionState res;
        res.version = TestProtocolVersion;
        res.cipher = CipherSuite::TLS_AES_128_GCM_SHA256;
        res.resumptionSecret = folly::IOBuf::copyBuffer("resumesecret");
        res.serverCert = cert_;
        res.handshakeTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(1));
        return std::make_pair(PskType::Resumption, std::move(res));
      }));
  Sequence contextSeq;
  mockKeyScheduler_ = new MockKeyScheduler();
  mockHandshakeContext_ = new MockHandshakeContext();
  EXPECT_CALL(*factory_, makeKeyScheduler(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs(
          [=]() { return std::unique_ptr<KeyScheduler>(mockKeyScheduler_); }));
  EXPECT_CALL(
      *factory_, makeHandshakeContext(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs([=]() {
        return std::unique_ptr<HandshakeContext>(mockHandshakeContext_);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_, deriveEarlySecret(RangeMatches("resumesecret")));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(BufMatches("client")))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getFinishedData(RangeMatches("bdr")))
      .InSequence(contextSeq)
      .WillRepeatedly(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(
      *mockHandshakeContext_, appendToTranscript(BufMatches("helloencoding")))
      .InSequence(contextSeq);
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(EarlySecrets::ResumptionPskBinder, RangeMatches("")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'b', 'd', 'r'}),
            EarlySecrets::ResumptionPskBinder);
      }));
  EXPECT_CALL(*factory_, makeKeyExchange(NamedGroup::x25519))
      .WillOnce(InvokeWithoutArgs([]() {
        auto ret = std::make_unique<MockKeyExchange>();
        EXPECT_CALL(*ret, generateKeyPair());
        EXPECT_CALL(*ret, generateSharedSecret(RangeMatches("keyshare")))
            .WillOnce(InvokeWithoutArgs(
                []() { return IOBuf::copyBuffer("sharedsecret"); }));
        EXPECT_CALL(*ret, getKeyShare()).WillOnce(InvokeWithoutArgs([]() {
          return IOBuf::copyBuffer("servershare");
        }));
        return ret;
      }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(Invoke([]() { return IOBuf::copyBuffer("chlo_shlo"); }));
  EXPECT_CALL(
      *mockKeyScheduler_, deriveHandshakeSecret(RangeMatches("sharedsecret")));
  EXPECT_CALL(*factory_, makeRandom()).WillOnce(Invoke([]() {
    Random random;
    random.fill(0x44);
    return random;
  }));
  EXPECT_CALL(*mockWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    EXPECT_EQ(msg.type, ContentType::handshake);
    auto shlo = TestMessages::serverHello();
    ServerPresharedKey serverPsk;
    serverPsk.selected_identity = 0;
    shlo.extensions.push_back(encodeExtension(std::move(serverPsk)));
    EXPECT_TRUE(IOBufEqualTo()(msg.fragment, encodeHandshake(std::move(shlo))));
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = mockWrite_->getEncryptionLevel();
    content.data = IOBuf::copyBuffer("writtenshlo");
    return content;
  }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ServerHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'h', 't'}),
            HandshakeSecrets::ServerHandshakeTraffic);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ClientHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'c', 'h', 't'}),
            HandshakeSecrets::ClientHandshakeTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverkey"),
                          IOBuf::copyBuffer("serveriv")};
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("cht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("clientkey"),
                          IOBuf::copyBuffer("clientiv")};
      }));
  MockAead* raead;
  MockAead* waead;
  MockAead* appwaead;
  MockEncryptedReadRecordLayer* rrl;
  MockEncryptedWriteRecordLayer* wrl;
  MockEncryptedWriteRecordLayer* appwrl;
  expectAeadCreation({{"clientkey", &raead},
                      {"serverkey", &waead},
                      {"serverappkey", &appwaead}});
  expectEncryptedReadRecordLayerCreation(
      &rrl, &raead, StringPiece("cht"), false);
  Sequence recSeq;
  expectEncryptedWriteRecordLayerCreation(
      &wrl,
      &waead,
      StringPiece("sht"),
      [](TLSMessage& msg, auto writeRecord) {
        EXPECT_EQ(msg.type, ContentType::handshake);
        EXPECT_TRUE(IOBufEqualTo()(
            msg.fragment,
            getEncryptedHandshakeWrite(
                TestMessages::encryptedExt(), TestMessages::finished())));
        TLSContent content;
        content.contentType = msg.type;
        content.encryptionLevel = writeRecord->getEncryptionLevel();
        content.data = folly::IOBuf::copyBuffer("handshake");
        return content;
      },
      &recSeq);
  expectEncryptedWriteRecordLayerCreation(
      &appwrl, &appwaead, StringPiece("sat"), nullptr, &recSeq);
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getFinishedData(RangeMatches("sht")))
      .InSequence(contextSeq)
      .WillRepeatedly(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(InvokeWithoutArgs(
          []() { return IOBuf::copyBuffer("chlo_shlo_sfin"); }));
  EXPECT_CALL(*mockKeyScheduler_, deriveMasterSecret());
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(MasterSecrets::ExporterMaster, RangeMatches("chlo_shlo_sfin")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'e', 'x', 'p', 'm'}),
            MasterSecrets::ExporterMaster);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      deriveAppTrafficSecrets(RangeMatches("chlo_shlo_sfin")));
  EXPECT_CALL(
      *mockKeyScheduler_, getSecret(AppTrafficSecrets::ServerAppTraffic))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'a', 't'}),
            AppTrafficSecrets::ServerAppTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sat"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverappkey"),
                          IOBuf::copyBuffer("serverappiv")};
      }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHelloPsk()));

  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  ASSERT_EQ(write.contents.size(), 2);
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::Plaintext);
  EXPECT_EQ(write.contents[0].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[0].data, IOBuf::copyBuffer("writtenshlo")));

  EXPECT_EQ(write.contents[1].encryptionLevel, EncryptionLevel::Handshake);
  EXPECT_EQ(write.contents[1].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[1].data, IOBuf::copyBuffer("handshake")));
  expectSecret(
      actions, HandshakeSecrets::ClientHandshakeTraffic, StringPiece("cht"));
  expectSecret(
      actions, HandshakeSecrets::ServerHandshakeTraffic, StringPiece("sht"));
  expectSecret(
      actions, AppTrafficSecrets::ServerAppTraffic, StringPiece("sat"));
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.readRecordLayer().get(), rrl);
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::Handshake);
  EXPECT_EQ(state_.writeRecordLayer().get(), appwrl);
  EXPECT_EQ(
      state_.writeRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::AppTraffic);
  EXPECT_EQ(state_.handshakeContext().get(), mockHandshakeContext_);
  EXPECT_EQ(state_.keyScheduler().get(), mockKeyScheduler_);
  EXPECT_EQ(state_.serverCert(), cert_);
  EXPECT_EQ(state_.cipher(), CipherSuite::TLS_AES_128_GCM_SHA256);
  EXPECT_EQ(state_.group(), NamedGroup::x25519);
  EXPECT_FALSE(state_.sigScheme().hasValue());
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.pskMode(), PskKeyExchangeMode::psk_dhe_ke);
  EXPECT_EQ(state_.keyExchangeType(), KeyExchangeType::OneRtt);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::NotAttempted);
  EXPECT_EQ(state_.replayCacheResult(), ReplayCacheResult::NotChecked);
  EXPECT_TRUE(state_.clientClockSkew().hasValue());
  EXPECT_EQ(*state_.alpn(), "h2");
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.clientHandshakeSecret(), IOBuf::copyBuffer("cht")));
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.exporterMasterSecret(), IOBuf::copyBuffer("expm")));
  EXPECT_FALSE(state_.earlyExporterMasterSecret().hasValue());
}

TEST_F(ServerProtocolTest, TestClientHelloHelloRetryRequestFlow) {
  setUpExpectingClientHello();
  auto firstHandshakeContext = new MockHandshakeContext();
  Sequence firstContextSeq;
  Sequence factorySeq;
  EXPECT_CALL(
      *factory_, makeHandshakeContext(CipherSuite::TLS_AES_128_GCM_SHA256))
      .InSequence(factorySeq)
      .WillOnce(InvokeWithoutArgs([=]() {
        return std::unique_ptr<HandshakeContext>(firstHandshakeContext);
      }));
  EXPECT_CALL(
      *firstHandshakeContext,
      appendToTranscript(BufMatches("clienthelloencoding")))
      .InSequence(firstContextSeq);
  EXPECT_CALL(*firstHandshakeContext, getHandshakeContext())
      .InSequence(firstContextSeq)
      .WillRepeatedly(Invoke([]() { return IOBuf::copyBuffer("chlo_hash"); }));
  auto secondHandshakeContext = new MockHandshakeContext();
  EXPECT_CALL(
      *factory_, makeHandshakeContext(CipherSuite::TLS_AES_128_GCM_SHA256))
      .InSequence(factorySeq)
      .WillOnce(InvokeWithoutArgs([=]() {
        return std::unique_ptr<HandshakeContext>(secondHandshakeContext);
      }));
  EXPECT_CALL(*secondHandshakeContext, appendToTranscript(_)).Times(2);
  EXPECT_CALL(*mockWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = mockWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::handshake);
    EXPECT_TRUE(IOBufEqualTo()(
        msg.fragment, encodeHandshake(TestMessages::helloRetryRequest())));
    content.data = IOBuf::copyBuffer("writtenhrr");
    return content;
  }));
  auto newRrl = new MockPlaintextReadRecordLayer();
  EXPECT_CALL(*factory_, makePlaintextReadRecordLayer())
      .WillOnce(Invoke([newRrl]() {
        return std::unique_ptr<PlaintextReadRecordLayer>(newRrl);
      }));
  EXPECT_CALL(*newRrl, setSkipEncryptedRecords(false));

  context_->setSupportedGroups({NamedGroup::secp256r1, NamedGroup::x25519});
  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHello()));

  expectActions<MutateState, WriteToSocket>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  ASSERT_EQ(write.contents.size(), 1);
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::Plaintext);
  EXPECT_EQ(write.contents[0].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[0].data, IOBuf::copyBuffer("writtenhrr")));
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingClientHello);
  EXPECT_EQ(state_.readRecordLayer().get(), newRrl);
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::Plaintext);
  EXPECT_EQ(state_.writeRecordLayer().get(), mockWrite_);
  EXPECT_EQ(
      state_.writeRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::Plaintext);
  EXPECT_EQ(state_.handshakeContext().get(), secondHandshakeContext);
  EXPECT_EQ(state_.version(), TestProtocolVersion);
  EXPECT_EQ(state_.cipher(), CipherSuite::TLS_AES_128_GCM_SHA256);
  EXPECT_EQ(state_.group(), NamedGroup::secp256r1);
  EXPECT_EQ(state_.keyExchangeType(), KeyExchangeType::HelloRetryRequest);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::NotAttempted);
  EXPECT_EQ(state_.replayCacheResult(), ReplayCacheResult::NotChecked);
  EXPECT_FALSE(state_.earlyExporterMasterSecret().hasValue());
}

TEST_F(ServerProtocolTest, TestRetryClientHelloFullHandshakeFlow) {
  setUpExpectingClientHelloRetry();
  Sequence contextSeq;
  mockKeyScheduler_ = new MockKeyScheduler();
  EXPECT_CALL(*factory_, makeKeyScheduler(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs(
          [=]() { return std::unique_ptr<KeyScheduler>(mockKeyScheduler_); }));
  EXPECT_CALL(
      *mockHandshakeContext_,
      appendToTranscript(BufMatches("clienthelloencoding")))
      .InSequence(contextSeq);
  EXPECT_CALL(*factory_, makeKeyExchange(NamedGroup::x25519))
      .WillOnce(InvokeWithoutArgs([]() {
        auto ret = std::make_unique<MockKeyExchange>();
        EXPECT_CALL(*ret, generateKeyPair());
        EXPECT_CALL(*ret, generateSharedSecret(RangeMatches("keyshare")))
            .WillOnce(InvokeWithoutArgs(
                []() { return IOBuf::copyBuffer("sharedsecret"); }));
        EXPECT_CALL(*ret, getKeyShare()).WillOnce(InvokeWithoutArgs([]() {
          return IOBuf::copyBuffer("servershare");
        }));
        return ret;
      }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(Invoke([]() { return IOBuf::copyBuffer("chlo_shlo"); }));
  EXPECT_CALL(
      *mockKeyScheduler_, deriveHandshakeSecret(RangeMatches("sharedsecret")));
  EXPECT_CALL(*factory_, makeRandom()).WillOnce(Invoke([]() {
    Random random;
    random.fill(0x44);
    return random;
  }));
  EXPECT_CALL(*mockWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = mockWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::handshake);
    EXPECT_TRUE(IOBufEqualTo()(
        msg.fragment, encodeHandshake(TestMessages::serverHello())));
    content.data = IOBuf::copyBuffer("writtenshlo");
    return content;
  }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ServerHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'h', 't'}),
            HandshakeSecrets::ServerHandshakeTraffic);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ClientHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'c', 'h', 't'}),
            HandshakeSecrets::ClientHandshakeTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverkey"),
                          IOBuf::copyBuffer("serveriv")};
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("cht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("clientkey"),
                          IOBuf::copyBuffer("clientiv")};
      }));
  MockAead* raead;
  MockAead* waead;
  MockAead* appwaead;
  MockEncryptedReadRecordLayer* rrl;
  MockEncryptedWriteRecordLayer* wrl;
  MockEncryptedWriteRecordLayer* appwrl;
  expectAeadCreation({{"clientkey", &raead},
                      {"serverkey", &waead},
                      {"serverappkey", &appwaead}});
  expectEncryptedReadRecordLayerCreation(
      &rrl, &raead, StringPiece("cht"), false);
  Sequence recSeq;
  expectEncryptedWriteRecordLayerCreation(
      &wrl,
      &waead,
      StringPiece("sht"),
      [](TLSMessage& msg, auto writeRecord) {
        EXPECT_EQ(msg.type, ContentType::handshake);
        EXPECT_TRUE(IOBufEqualTo()(
            msg.fragment,
            getEncryptedHandshakeWrite(
                TestMessages::encryptedExt(),
                TestMessages::certificate(),
                TestMessages::certificateVerify(),
                TestMessages::finished())));
        TLSContent content;
        content.contentType = msg.type;
        content.encryptionLevel = writeRecord->getEncryptionLevel();
        content.data = folly::IOBuf::copyBuffer("handshake");
        return content;
      },
      &recSeq);
  expectEncryptedWriteRecordLayerCreation(
      &appwrl, &appwaead, StringPiece("sat"), nullptr, &recSeq);
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*certManager_, getCert(_, _, _))
      .WillOnce(Invoke(
          [=](const folly::Optional<std::string>& sni,
              const std::vector<SignatureScheme>& /* supportedSigSchemes */,
              const std::vector<SignatureScheme>& peerSigSchemes) {
            EXPECT_EQ(*sni, "www.hostname.com");
            EXPECT_EQ(peerSigSchemes.size(), 2);
            EXPECT_EQ(
                peerSigSchemes[0], SignatureScheme::ecdsa_secp256r1_sha256);
            EXPECT_EQ(peerSigSchemes[1], SignatureScheme::rsa_pss_sha256);
            return CertManager::CertMatch(
                std::make_pair(cert_, SignatureScheme::ecdsa_secp256r1_sha256));
          }));
  EXPECT_CALL(*cert_, _getCertMessage(_));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(
          Invoke([]() { return IOBuf::copyBuffer("chlo_shlo_ee_cert"); }));
  EXPECT_CALL(
      *cert_,
      sign(
          SignatureScheme::ecdsa_secp256r1_sha256,
          CertificateVerifyContext::Server,
          RangeMatches("chlo_shlo_ee_cert")))
      .WillOnce(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("signature"); }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getFinishedData(RangeMatches("sht")))
      .InSequence(contextSeq)
      .WillRepeatedly(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(InvokeWithoutArgs(
          []() { return IOBuf::copyBuffer("chlo_shlo_ee_cert_sfin"); }));
  EXPECT_CALL(*mockKeyScheduler_, deriveMasterSecret());
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          MasterSecrets::ExporterMaster,
          RangeMatches("chlo_shlo_ee_cert_sfin")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'e', 'x', 'p', 'm'}),
            MasterSecrets::ExporterMaster);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      deriveAppTrafficSecrets(RangeMatches("chlo_shlo_ee_cert_sfin")));
  EXPECT_CALL(
      *mockKeyScheduler_, getSecret(AppTrafficSecrets::ServerAppTraffic))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'a', 't'}),
            AppTrafficSecrets::ServerAppTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sat"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverappkey"),
                          IOBuf::copyBuffer("serverappiv")};
      }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHello()));

  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  ASSERT_EQ(write.contents.size(), 2);
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::Plaintext);
  EXPECT_EQ(write.contents[0].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[0].data, IOBuf::copyBuffer("writtenshlo")));

  EXPECT_EQ(write.contents[1].encryptionLevel, EncryptionLevel::Handshake);
  EXPECT_EQ(write.contents[1].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[1].data, IOBuf::copyBuffer("handshake")));
  expectSecret(
      actions, HandshakeSecrets::ClientHandshakeTraffic, StringPiece("cht"));
  expectSecret(
      actions, HandshakeSecrets::ServerHandshakeTraffic, StringPiece("sht"));
  expectSecret(
      actions, AppTrafficSecrets::ServerAppTraffic, StringPiece("sat"));
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.readRecordLayer().get(), rrl);
  EXPECT_EQ(state_.writeRecordLayer().get(), appwrl);
  EXPECT_EQ(state_.handshakeContext().get(), mockHandshakeContext_);
  EXPECT_EQ(state_.keyScheduler().get(), mockKeyScheduler_);
  EXPECT_EQ(state_.serverCert(), cert_);
  EXPECT_EQ(state_.version(), TestProtocolVersion);
  EXPECT_EQ(state_.cipher(), CipherSuite::TLS_AES_128_GCM_SHA256);
  EXPECT_EQ(state_.group(), NamedGroup::x25519);
  EXPECT_EQ(state_.sigScheme(), SignatureScheme::ecdsa_secp256r1_sha256);
  EXPECT_EQ(state_.pskType(), PskType::NotAttempted);
  EXPECT_FALSE(state_.pskMode().hasValue());
  EXPECT_EQ(state_.keyExchangeType(), KeyExchangeType::HelloRetryRequest);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::NotAttempted);
  EXPECT_EQ(state_.replayCacheResult(), ReplayCacheResult::NotChecked);
  EXPECT_FALSE(state_.clientClockSkew().hasValue());
  EXPECT_EQ(*state_.alpn(), "h2");
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.clientHandshakeSecret(), IOBuf::copyBuffer("cht")));
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.exporterMasterSecret(), IOBuf::copyBuffer("expm")));
  EXPECT_FALSE(state_.earlyExporterMasterSecret().hasValue());
}

TEST_F(ServerProtocolTest, TestRetryClientHelloPskDheFlow) {
  context_->setSupportedPskModes({PskKeyExchangeMode::psk_dhe_ke});
  setUpExpectingClientHelloRetry();
  EXPECT_CALL(*mockTicketCipher_, _decrypt(_))
      .WillOnce(InvokeWithoutArgs([=]() {
        ResumptionState res;
        res.version = TestProtocolVersion;
        res.cipher = CipherSuite::TLS_AES_128_GCM_SHA256;
        res.resumptionSecret = folly::IOBuf::copyBuffer("resumesecret");
        res.serverCert = cert_;
        res.handshakeTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(1));
        return std::make_pair(PskType::Resumption, std::move(res));
      }));
  Sequence contextSeq;
  mockKeyScheduler_ = new MockKeyScheduler();
  EXPECT_CALL(*factory_, makeKeyScheduler(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs(
          [=]() { return std::unique_ptr<KeyScheduler>(mockKeyScheduler_); }));
  EXPECT_CALL(
      *mockKeyScheduler_, deriveEarlySecret(RangeMatches("resumesecret")));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(BufMatches("client")))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getFinishedData(RangeMatches("bdr")))
      .InSequence(contextSeq)
      .WillRepeatedly(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(
      *mockHandshakeContext_, appendToTranscript(BufMatches("helloencoding")))
      .InSequence(contextSeq);
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(EarlySecrets::ResumptionPskBinder, RangeMatches("")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'b', 'd', 'r'}),
            EarlySecrets::ResumptionPskBinder);
      }));
  EXPECT_CALL(*factory_, makeKeyExchange(NamedGroup::x25519))
      .WillOnce(InvokeWithoutArgs([]() {
        auto ret = std::make_unique<MockKeyExchange>();
        EXPECT_CALL(*ret, generateKeyPair());
        EXPECT_CALL(*ret, generateSharedSecret(RangeMatches("keyshare")))
            .WillOnce(InvokeWithoutArgs(
                []() { return IOBuf::copyBuffer("sharedsecret"); }));
        EXPECT_CALL(*ret, getKeyShare()).WillOnce(InvokeWithoutArgs([]() {
          return IOBuf::copyBuffer("servershare");
        }));
        return ret;
      }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(Invoke([]() { return IOBuf::copyBuffer("chlo_shlo"); }));
  EXPECT_CALL(
      *mockKeyScheduler_, deriveHandshakeSecret(RangeMatches("sharedsecret")));
  EXPECT_CALL(*factory_, makeRandom()).WillOnce(Invoke([]() {
    Random random;
    random.fill(0x44);
    return random;
  }));
  EXPECT_CALL(*mockWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = mockWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::handshake);
    auto shlo = TestMessages::serverHello();
    ServerPresharedKey serverPsk;
    serverPsk.selected_identity = 0;
    shlo.extensions.push_back(encodeExtension(std::move(serverPsk)));
    EXPECT_TRUE(IOBufEqualTo()(msg.fragment, encodeHandshake(std::move(shlo))));
    content.data = IOBuf::copyBuffer("writtenshlo");
    return content;
  }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ServerHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'h', 't'}),
            HandshakeSecrets::ServerHandshakeTraffic);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ClientHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'c', 'h', 't'}),
            HandshakeSecrets::ClientHandshakeTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverkey"),
                          IOBuf::copyBuffer("serveriv")};
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("cht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("clientkey"),
                          IOBuf::copyBuffer("clientiv")};
      }));
  MockAead* raead;
  MockAead* waead;
  MockAead* appwaead;
  MockEncryptedReadRecordLayer* rrl;
  MockEncryptedWriteRecordLayer* wrl;
  MockEncryptedWriteRecordLayer* appwrl;
  expectAeadCreation({{"clientkey", &raead},
                      {"serverkey", &waead},
                      {"serverappkey", &appwaead}});
  expectEncryptedReadRecordLayerCreation(
      &rrl, &raead, StringPiece("cht"), false);
  Sequence recSeq;
  expectEncryptedWriteRecordLayerCreation(
      &wrl,
      &waead,
      StringPiece("sht"),
      [](TLSMessage& msg, auto writeRecord) {
        EXPECT_EQ(msg.type, ContentType::handshake);
        EXPECT_TRUE(IOBufEqualTo()(
            msg.fragment,
            getEncryptedHandshakeWrite(
                TestMessages::encryptedExt(), TestMessages::finished())));
        TLSContent content;
        content.contentType = msg.type;
        content.encryptionLevel = writeRecord->getEncryptionLevel();
        content.data = folly::IOBuf::copyBuffer("handshake");
        return content;
      },
      &recSeq);
  expectEncryptedWriteRecordLayerCreation(
      &appwrl, &appwaead, StringPiece("sat"), nullptr, &recSeq);
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getFinishedData(RangeMatches("sht")))
      .InSequence(contextSeq)
      .WillRepeatedly(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(InvokeWithoutArgs(
          []() { return IOBuf::copyBuffer("chlo_shlo_sfin"); }));
  EXPECT_CALL(*mockKeyScheduler_, deriveMasterSecret());
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(MasterSecrets::ExporterMaster, RangeMatches("chlo_shlo_sfin")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'e', 'x', 'p', 'm'}),
            MasterSecrets::ExporterMaster);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      deriveAppTrafficSecrets(RangeMatches("chlo_shlo_sfin")));
  EXPECT_CALL(
      *mockKeyScheduler_, getSecret(AppTrafficSecrets::ServerAppTraffic))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'a', 't'}),
            AppTrafficSecrets::ServerAppTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sat"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverappkey"),
                          IOBuf::copyBuffer("serverappiv")};
      }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHelloPsk()));

  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  ASSERT_EQ(write.contents.size(), 2);
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::Plaintext);
  EXPECT_EQ(write.contents[0].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[0].data, IOBuf::copyBuffer("writtenshlo")));

  EXPECT_EQ(write.contents[1].encryptionLevel, EncryptionLevel::Handshake);
  EXPECT_EQ(write.contents[1].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[1].data, IOBuf::copyBuffer("handshake")));
  expectSecret(
      actions, HandshakeSecrets::ClientHandshakeTraffic, StringPiece("cht"));
  expectSecret(
      actions, HandshakeSecrets::ServerHandshakeTraffic, StringPiece("sht"));
  expectSecret(
      actions, AppTrafficSecrets::ServerAppTraffic, StringPiece("sat"));
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.readRecordLayer().get(), rrl);
  EXPECT_EQ(state_.writeRecordLayer().get(), appwrl);
  EXPECT_EQ(state_.handshakeContext().get(), mockHandshakeContext_);
  EXPECT_EQ(state_.keyScheduler().get(), mockKeyScheduler_);
  EXPECT_EQ(state_.serverCert(), cert_);
  EXPECT_EQ(state_.cipher(), CipherSuite::TLS_AES_128_GCM_SHA256);
  EXPECT_EQ(state_.group(), NamedGroup::x25519);
  EXPECT_FALSE(state_.sigScheme().hasValue());
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.pskMode(), PskKeyExchangeMode::psk_dhe_ke);
  EXPECT_EQ(state_.keyExchangeType(), KeyExchangeType::HelloRetryRequest);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::NotAttempted);
  EXPECT_EQ(state_.replayCacheResult(), ReplayCacheResult::NotChecked);
  EXPECT_TRUE(state_.clientClockSkew().hasValue());
  EXPECT_EQ(*state_.alpn(), "h2");
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.clientHandshakeSecret(), IOBuf::copyBuffer("cht")));
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.exporterMasterSecret(), IOBuf::copyBuffer("expm")));
  EXPECT_FALSE(state_.earlyExporterMasterSecret().hasValue());
}

TEST_F(ServerProtocolTest, TestClientHelloPskDheEarlyFlow) {
  context_->setSupportedPskModes({PskKeyExchangeMode::psk_dhe_ke});
  acceptEarlyData();
  setUpExpectingClientHello();
  EXPECT_CALL(*mockTicketCipher_, _decrypt(_))
      .WillOnce(InvokeWithoutArgs([=]() {
        ResumptionState res;
        res.version = TestProtocolVersion;
        res.cipher = CipherSuite::TLS_AES_128_GCM_SHA256;
        res.resumptionSecret = folly::IOBuf::copyBuffer("resumesecret");
        res.serverCert = cert_;
        res.alpn = "h2";
        res.ticketAgeAdd = 0;
        res.ticketIssueTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(10));
        res.handshakeTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(1));
        return std::make_pair(PskType::Resumption, std::move(res));
      }));
  Sequence contextSeq;
  mockKeyScheduler_ = new MockKeyScheduler();
  mockHandshakeContext_ = new MockHandshakeContext();
  EXPECT_CALL(*factory_, makeKeyScheduler(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs(
          [=]() { return std::unique_ptr<KeyScheduler>(mockKeyScheduler_); }));
  EXPECT_CALL(
      *factory_, makeHandshakeContext(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs([=]() {
        return std::unique_ptr<HandshakeContext>(mockHandshakeContext_);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_, deriveEarlySecret(RangeMatches("resumesecret")));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(BufMatches("client")))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getFinishedData(RangeMatches("bdr")))
      .InSequence(contextSeq)
      .WillRepeatedly(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(
      *mockHandshakeContext_, appendToTranscript(BufMatches("helloencoding")))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(Invoke([]() { return IOBuf::copyBuffer("chlo"); }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(EarlySecrets::ResumptionPskBinder, RangeMatches("")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'b', 'd', 'r'}),
            EarlySecrets::ResumptionPskBinder);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(EarlySecrets::ClientEarlyTraffic, RangeMatches("chlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'c', 'e', 't'}),
            EarlySecrets::ClientEarlyTraffic);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(EarlySecrets::EarlyExporter, RangeMatches("chlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'e', 'e', 'm'}), EarlySecrets::EarlyExporter);
      }));
  EXPECT_CALL(*factory_, makeKeyExchange(NamedGroup::x25519))
      .WillOnce(InvokeWithoutArgs([]() {
        auto ret = std::make_unique<MockKeyExchange>();
        EXPECT_CALL(*ret, generateKeyPair());
        EXPECT_CALL(*ret, generateSharedSecret(RangeMatches("keyshare")))
            .WillOnce(InvokeWithoutArgs(
                []() { return IOBuf::copyBuffer("sharedsecret"); }));
        EXPECT_CALL(*ret, getKeyShare()).WillOnce(InvokeWithoutArgs([]() {
          return IOBuf::copyBuffer("servershare");
        }));
        return ret;
      }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(Invoke([]() { return IOBuf::copyBuffer("chlo_shlo"); }));
  EXPECT_CALL(
      *mockKeyScheduler_, deriveHandshakeSecret(RangeMatches("sharedsecret")));
  EXPECT_CALL(*factory_, makeRandom()).WillOnce(Invoke([]() {
    Random random;
    random.fill(0x44);
    return random;
  }));
  EXPECT_CALL(*mockWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = mockWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::handshake);
    auto shlo = TestMessages::serverHello();
    ServerPresharedKey serverPsk;
    serverPsk.selected_identity = 0;
    shlo.extensions.push_back(encodeExtension(std::move(serverPsk)));
    EXPECT_TRUE(IOBufEqualTo()(msg.fragment, encodeHandshake(std::move(shlo))));
    content.data = IOBuf::copyBuffer("writtenshlo");
    return content;
  }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ServerHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'h', 't'}),
            HandshakeSecrets::ServerHandshakeTraffic);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ClientHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'c', 'h', 't'}),
            HandshakeSecrets::ClientHandshakeTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("cet"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("earlykey"),
                          IOBuf::copyBuffer("earlyiv")};
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverkey"),
                          IOBuf::copyBuffer("serveriv")};
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("cht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("clientkey"),
                          IOBuf::copyBuffer("clientiv")};
      }));
  MockAead* earlyaead;
  MockAead* raead;
  MockAead* waead;
  MockAead* appwaead;
  MockEncryptedReadRecordLayer* earlyrrl;
  MockEncryptedReadRecordLayer* handshakerrl;
  MockEncryptedWriteRecordLayer* wrl;
  MockEncryptedWriteRecordLayer* appwrl;
  expectAeadCreation({{"earlykey", &earlyaead},
                      {"clientkey", &raead},
                      {"serverkey", &waead},
                      {"serverappkey", &appwaead}});
  Sequence readRecSeq;
  expectEncryptedReadRecordLayerCreation(
      &earlyrrl, &earlyaead, StringPiece("cet"), folly::none, &readRecSeq);
  expectEncryptedReadRecordLayerCreation(
      &handshakerrl, &raead, StringPiece("cht"), false, &readRecSeq);
  Sequence recSeq;
  expectEncryptedWriteRecordLayerCreation(
      &wrl,
      &waead,
      StringPiece("sht"),
      [](TLSMessage& msg, auto writeRecord) {
        EXPECT_EQ(msg.type, ContentType::handshake);
        EXPECT_TRUE(IOBufEqualTo()(
            msg.fragment,
            getEncryptedHandshakeWrite(
                TestMessages::encryptedExtEarly(), TestMessages::finished())));
        TLSContent content;
        content.contentType = msg.type;
        content.encryptionLevel = writeRecord->getEncryptionLevel();
        content.data = folly::IOBuf::copyBuffer("handshake");
        return content;
      },
      &recSeq);
  expectEncryptedWriteRecordLayerCreation(
      &appwrl, &appwaead, StringPiece("sat"), nullptr, &recSeq);
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getFinishedData(RangeMatches("sht")))
      .InSequence(contextSeq)
      .WillRepeatedly(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(InvokeWithoutArgs(
          []() { return IOBuf::copyBuffer("chlo_shlo_sfin"); }));
  EXPECT_CALL(*mockKeyScheduler_, deriveMasterSecret());
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(MasterSecrets::ExporterMaster, RangeMatches("chlo_shlo_sfin")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'e', 'x', 'p', 'm'}),
            MasterSecrets::ExporterMaster);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      deriveAppTrafficSecrets(RangeMatches("chlo_shlo_sfin")));
  EXPECT_CALL(
      *mockKeyScheduler_, getSecret(AppTrafficSecrets::ServerAppTraffic))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'a', 't'}),
            AppTrafficSecrets::ServerAppTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sat"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverappkey"),
                          IOBuf::copyBuffer("serverappiv")};
      }));

  std::chrono::milliseconds age =
      std::chrono::minutes(5) - std::chrono::seconds(10);
  auto actions = getActions(detail::processEvent(
      state_, TestMessages::clientHelloPskEarly(age.count())));

  expectActions<
      MutateState,
      WriteToSocket,
      ReportEarlyHandshakeSuccess,
      SecretAvailable>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  ASSERT_EQ(write.contents.size(), 2);
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::Plaintext);
  EXPECT_EQ(write.contents[0].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[0].data, IOBuf::copyBuffer("writtenshlo")));

  EXPECT_EQ(write.contents[1].encryptionLevel, EncryptionLevel::Handshake);
  EXPECT_EQ(write.contents[1].contentType, ContentType::handshake);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[1].data, IOBuf::copyBuffer("handshake")));
  expectSecret(
      actions, EarlySecrets::ClientEarlyTraffic, folly::StringPiece("cet"));
  expectSecret(
      actions, HandshakeSecrets::ClientHandshakeTraffic, StringPiece("cht"));
  expectSecret(
      actions, HandshakeSecrets::ServerHandshakeTraffic, StringPiece("sht"));
  expectSecret(
      actions, AppTrafficSecrets::ServerAppTraffic, StringPiece("sat"));
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::AcceptingEarlyData);
  EXPECT_EQ(
      *state_.handshakeTime(),
      std::chrono::system_clock::time_point(std::chrono::seconds(1)));
  EXPECT_EQ(state_.handshakeReadRecordLayer().get(), handshakerrl);
  EXPECT_EQ(state_.readRecordLayer().get(), earlyrrl);
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::EarlyData);
  EXPECT_EQ(state_.writeRecordLayer().get(), appwrl);
  EXPECT_EQ(
      state_.writeRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::AppTraffic);
  EXPECT_EQ(state_.handshakeContext().get(), mockHandshakeContext_);
  EXPECT_EQ(state_.keyScheduler().get(), mockKeyScheduler_);
  EXPECT_EQ(state_.serverCert(), cert_);
  EXPECT_EQ(state_.cipher(), CipherSuite::TLS_AES_128_GCM_SHA256);
  EXPECT_EQ(state_.group(), NamedGroup::x25519);
  EXPECT_FALSE(state_.sigScheme().hasValue());
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.pskMode(), PskKeyExchangeMode::psk_dhe_ke);
  EXPECT_EQ(state_.keyExchangeType(), KeyExchangeType::OneRtt);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Accepted);
  EXPECT_EQ(state_.replayCacheResult(), ReplayCacheResult::NotReplay);
  EXPECT_TRUE(state_.clientClockSkew().hasValue());
  EXPECT_EQ(*state_.alpn(), "h2");
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.clientHandshakeSecret(), IOBuf::copyBuffer("cht")));
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.exporterMasterSecret(), IOBuf::copyBuffer("expm")));
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.earlyExporterMasterSecret(), IOBuf::copyBuffer("eem")));
}

TEST_F(ServerProtocolTest, TestClientHelloPskEarlyFlow) {
  context_->setSupportedPskModes({PskKeyExchangeMode::psk_ke});
  acceptEarlyData();
  setUpExpectingClientHello();
  EXPECT_CALL(*mockTicketCipher_, _decrypt(_))
      .WillOnce(InvokeWithoutArgs([=]() {
        ResumptionState res;
        res.version = TestProtocolVersion;
        res.cipher = CipherSuite::TLS_AES_128_GCM_SHA256;
        res.resumptionSecret = folly::IOBuf::copyBuffer("resumesecret");
        res.serverCert = cert_;
        res.alpn = "h2";
        res.ticketAgeAdd = 0;
        res.ticketIssueTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(10));
        res.handshakeTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(1));
        return std::make_pair(PskType::Resumption, std::move(res));
      }));
  Sequence contextSeq;
  mockKeyScheduler_ = new MockKeyScheduler();
  mockHandshakeContext_ = new MockHandshakeContext();
  EXPECT_CALL(*factory_, makeKeyScheduler(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs(
          [=]() { return std::unique_ptr<KeyScheduler>(mockKeyScheduler_); }));
  EXPECT_CALL(
      *factory_, makeHandshakeContext(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs([=]() {
        return std::unique_ptr<HandshakeContext>(mockHandshakeContext_);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_, deriveEarlySecret(RangeMatches("resumesecret")));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(BufMatches("client")))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getFinishedData(RangeMatches("bdr")))
      .InSequence(contextSeq)
      .WillRepeatedly(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(
      *mockHandshakeContext_, appendToTranscript(BufMatches("helloencoding")))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(Invoke([]() { return IOBuf::copyBuffer("chlo"); }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(EarlySecrets::ResumptionPskBinder, RangeMatches("")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'b', 'd', 'r'}),
            EarlySecrets::ResumptionPskBinder);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(EarlySecrets::ClientEarlyTraffic, RangeMatches("chlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'c', 'e', 't'}),
            EarlySecrets::ClientEarlyTraffic);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(EarlySecrets::EarlyExporter, RangeMatches("chlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'e', 'e', 'm'}), EarlySecrets::EarlyExporter);
      }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(Invoke([]() { return IOBuf::copyBuffer("chlo_shlo"); }));
  EXPECT_CALL(*mockKeyScheduler_, deriveHandshakeSecret());
  EXPECT_CALL(*factory_, makeRandom()).WillOnce(Invoke([]() {
    Random random;
    random.fill(0x44);
    return random;
  }));
  EXPECT_CALL(*mockWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = mockWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::handshake);
    auto shlo = TestMessages::serverHello();
    TestMessages::removeExtension(shlo, ExtensionType::key_share);
    ServerPresharedKey serverPsk;
    serverPsk.selected_identity = 0;
    shlo.extensions.push_back(encodeExtension(std::move(serverPsk)));
    EXPECT_TRUE(IOBufEqualTo()(msg.fragment, encodeHandshake(std::move(shlo))));
    content.data = IOBuf::copyBuffer("writtenshlo");
    return content;
  }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ServerHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'h', 't'}),
            HandshakeSecrets::ServerHandshakeTraffic);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(
          HandshakeSecrets::ClientHandshakeTraffic, RangeMatches("chlo_shlo")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'c', 'h', 't'}),
            HandshakeSecrets::ClientHandshakeTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("cet"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("earlykey"),
                          IOBuf::copyBuffer("earlyiv")};
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverkey"),
                          IOBuf::copyBuffer("serveriv")};
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("cht"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("clientkey"),
                          IOBuf::copyBuffer("clientiv")};
      }));
  MockAead* earlyaead;
  MockAead* raead;
  MockAead* waead;
  MockAead* appwaead;
  MockEncryptedReadRecordLayer* earlyrrl;
  MockEncryptedReadRecordLayer* handshakerrl;
  MockEncryptedWriteRecordLayer* wrl;
  MockEncryptedWriteRecordLayer* appwrl;
  expectAeadCreation({{"earlykey", &earlyaead},
                      {"clientkey", &raead},
                      {"serverkey", &waead},
                      {"serverappkey", &appwaead}});
  Sequence readRecSeq;
  expectEncryptedReadRecordLayerCreation(
      &earlyrrl, &earlyaead, StringPiece("cet"), folly::none, &readRecSeq);
  expectEncryptedReadRecordLayerCreation(
      &handshakerrl, &raead, StringPiece("cht"), false, &readRecSeq);
  Sequence recSeq;
  expectEncryptedWriteRecordLayerCreation(
      &wrl,
      &waead,
      StringPiece("sht"),
      [](TLSMessage& msg, auto writeRecord) {
        EXPECT_EQ(msg.type, ContentType::handshake);
        EXPECT_TRUE(IOBufEqualTo()(
            msg.fragment,
            getEncryptedHandshakeWrite(
                TestMessages::encryptedExtEarly(), TestMessages::finished())));
        TLSContent content;
        content.contentType = msg.type;
        content.encryptionLevel = writeRecord->getEncryptionLevel();
        content.data = folly::IOBuf::copyBuffer("handshake");
        return content;
      },
      &recSeq);
  expectEncryptedWriteRecordLayerCreation(
      &appwrl, &appwaead, StringPiece("sat"), nullptr, &recSeq);
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getFinishedData(RangeMatches("sht")))
      .InSequence(contextSeq)
      .WillRepeatedly(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(*mockHandshakeContext_, appendToTranscript(_))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(InvokeWithoutArgs(
          []() { return IOBuf::copyBuffer("chlo_shlo_sfin"); }));
  EXPECT_CALL(*mockKeyScheduler_, deriveMasterSecret());
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(MasterSecrets::ExporterMaster, RangeMatches("chlo_shlo_sfin")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'e', 'x', 'p', 'm'}),
            MasterSecrets::ExporterMaster);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      deriveAppTrafficSecrets(RangeMatches("chlo_shlo_sfin")));
  EXPECT_CALL(
      *mockKeyScheduler_, getSecret(AppTrafficSecrets::ServerAppTraffic))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'a', 't'}),
            AppTrafficSecrets::ServerAppTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sat"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverappkey"),
                          IOBuf::copyBuffer("serverappiv")};
      }));

  std::chrono::milliseconds age =
      std::chrono::minutes(5) - std::chrono::seconds(10);
  auto actions = getActions(detail::processEvent(
      state_, TestMessages::clientHelloPskEarly(age.count())));

  expectActions<
      MutateState,
      WriteToSocket,
      ReportEarlyHandshakeSuccess,
      SecretAvailable>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  expectSecret(
      actions, EarlySecrets::ClientEarlyTraffic, folly::StringPiece("cet"));
  expectSecret(
      actions, HandshakeSecrets::ClientHandshakeTraffic, StringPiece("cht"));
  expectSecret(
      actions, HandshakeSecrets::ServerHandshakeTraffic, StringPiece("sht"));
  expectSecret(
      actions, AppTrafficSecrets::ServerAppTraffic, StringPiece("sat"));
  ASSERT_EQ(write.contents.size(), 2);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[0].data, IOBuf::copyBuffer("writtenshlo")));
  EXPECT_EQ(write.contents[0].contentType, ContentType::handshake);
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::Plaintext);

  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[1].data, IOBuf::copyBuffer("handshake")));
  EXPECT_EQ(write.contents[1].contentType, ContentType::handshake);
  EXPECT_EQ(write.contents[1].encryptionLevel, EncryptionLevel::Handshake);

  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::AcceptingEarlyData);
  EXPECT_EQ(
      *state_.handshakeTime(),
      std::chrono::system_clock::time_point(std::chrono::seconds(1)));
  EXPECT_EQ(state_.handshakeReadRecordLayer().get(), handshakerrl);
  EXPECT_EQ(state_.readRecordLayer().get(), earlyrrl);
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::EarlyData);
  EXPECT_EQ(state_.writeRecordLayer().get(), appwrl);
  EXPECT_EQ(
      state_.writeRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::AppTraffic);
  EXPECT_EQ(state_.handshakeContext().get(), mockHandshakeContext_);
  EXPECT_EQ(state_.keyScheduler().get(), mockKeyScheduler_);
  EXPECT_EQ(state_.serverCert(), cert_);
  EXPECT_EQ(state_.version(), TestProtocolVersion);
  EXPECT_EQ(state_.cipher(), CipherSuite::TLS_AES_128_GCM_SHA256);
  EXPECT_FALSE(state_.group().hasValue());
  EXPECT_FALSE(state_.sigScheme().hasValue());
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.pskMode(), PskKeyExchangeMode::psk_ke);
  EXPECT_EQ(state_.keyExchangeType(), KeyExchangeType::None);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Accepted);
  EXPECT_EQ(state_.replayCacheResult(), ReplayCacheResult::NotReplay);
  EXPECT_TRUE(state_.clientClockSkew().hasValue());
  EXPECT_EQ(*state_.alpn(), "h2");
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.clientHandshakeSecret(), IOBuf::copyBuffer("cht")));
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.exporterMasterSecret(), IOBuf::copyBuffer("expm")));
  EXPECT_TRUE(IOBufEqualTo()(
      *state_.earlyExporterMasterSecret(), IOBuf::copyBuffer("eem")));
}

TEST_F(ServerProtocolTest, TestClientHelloNullExtensions) {
  addExtensions_ = false;
  setUpExpectingClientHello();
  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHello()));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_CALL(*extensions_, getExtensions(_)).Times(0);
}

TEST_F(ServerProtocolTest, TestClientHelloLegacySessionId) {
  setUpExpectingClientHello();
  auto chloWithLegacy = TestMessages::clientHello();
  chloWithLegacy.legacy_session_id = IOBuf::copyBuffer("middleboxes");
  auto actions =
      getActions(detail::processEvent(state_, std::move(chloWithLegacy)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  EXPECT_EQ(write.contents.size(), 3);
  EXPECT_EQ(write.contents[0].contentType, ContentType::handshake);
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::Plaintext);
  EXPECT_EQ(write.contents[1].contentType, ContentType::change_cipher_spec);
  EXPECT_EQ(write.contents[1].encryptionLevel, EncryptionLevel::Plaintext);
  EXPECT_EQ(write.contents[2].contentType, ContentType::handshake);
  EXPECT_EQ(write.contents[2].encryptionLevel, EncryptionLevel::Handshake);
}

TEST_F(ServerProtocolTest, TestClientHelloLegacyHrr) {
  setUpExpectingClientHello();
  auto chloWithLegacy = TestMessages::clientHello();
  chloWithLegacy.legacy_session_id = IOBuf::copyBuffer("middleboxes");
  context_->setSupportedGroups({NamedGroup::secp256r1, NamedGroup::x25519});
  auto actions =
      getActions(detail::processEvent(state_, std::move(chloWithLegacy)));
  expectActions<MutateState, WriteToSocket>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  EXPECT_EQ(write.contents.size(), 2);
  EXPECT_EQ(write.contents[0].contentType, ContentType::handshake);
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::Plaintext);
  EXPECT_EQ(write.contents[1].contentType, ContentType::change_cipher_spec);
  EXPECT_EQ(write.contents[1].encryptionLevel, EncryptionLevel::Plaintext);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingClientHello);
}

TEST_F(ServerProtocolTest, TestClientHelloFullHandshake) {
  setUpExpectingClientHello();
  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHello()));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
}

TEST_F(ServerProtocolTest, TestClientHelloPsk) {
  context_->setSupportedPskModes({PskKeyExchangeMode::psk_ke});
  setUpExpectingClientHello();
  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHelloPsk()));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
}

TEST_F(ServerProtocolTest, TestClientHelloPskDhe) {
  context_->setSupportedPskModes({PskKeyExchangeMode::psk_dhe_ke});
  setUpExpectingClientHello();
  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHelloPsk()));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
}

TEST_F(ServerProtocolTest, TestClientHelloPskModeMismatch) {
  setUpExpectingClientHello();
  auto chlo = TestMessages::clientHello();
  TestMessages::removeExtension(chlo, ExtensionType::psk_key_exchange_modes);
  PskKeyExchangeModes modes;
  chlo.extensions.push_back(encodeExtension(std::move(modes)));
  TestMessages::addPsk(chlo);
  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
}

TEST_F(ServerProtocolTest, TestClientHelloNoSni) {
  setUpExpectingClientHello();
  auto chlo = TestMessages::clientHello();
  TestMessages::removeExtension(chlo, ExtensionType::server_name);
  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
}

TEST_F(ServerProtocolTest, TestClientHelloFullHandshakeRejectedPsk) {
  setUpExpectingClientHello();
  EXPECT_CALL(*mockTicketCipher_, _decrypt(_)).WillOnce(InvokeWithoutArgs([]() {
    return std::make_pair(PskType::Rejected, none);
  }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHelloPsk()));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Rejected);
}

TEST_F(ServerProtocolTest, TestClientHelloPskNoModes) {
  setUpExpectingClientHello();
  auto chlo = TestMessages::clientHelloPsk();
  TestMessages::removeExtension(chlo, ExtensionType::psk_key_exchange_modes);
  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectError<FizzException>(
      actions, AlertDescription::missing_extension, "no psk modes");
}

TEST_F(ServerProtocolTest, TestClientHelloPskNotSupported) {
  setUpExpectingClientHello();
  auto chlo = TestMessages::clientHello();
  TestMessages::removeExtension(chlo, ExtensionType::psk_key_exchange_modes);
  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::NotSupported);
  EXPECT_FALSE(state_.pskMode().hasValue());
}

TEST_F(ServerProtocolTest, TestClientHelloPskBadBinder) {
  setUpExpectingClientHello();
  auto chlo = TestMessages::clientHelloPsk();
  TestMessages::removeExtension(chlo, ExtensionType::pre_shared_key);
  ClientPresharedKey cpk;
  PskIdentity ident;
  ident.psk_identity = folly::IOBuf::copyBuffer("ident");
  cpk.identities.push_back(std::move(ident));
  PskBinder binder;
  binder.binder = folly::IOBuf::copyBuffer("verifyxxxx");
  cpk.binders.push_back(std::move(binder));
  chlo.extensions.push_back(encodeExtension(std::move(cpk)));
  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectError<FizzException>(
      actions, AlertDescription::bad_record_mac, "binder does not match");
}

TEST_F(ServerProtocolTest, TestClientHelloFallback) {
  context_->setVersionFallbackEnabled(true);
  setUpExpectingClientHello();
  auto clientHello = TestMessages::clientHello();
  TestMessages::removeExtension(clientHello, ExtensionType::supported_versions);
  auto actions =
      getActions(detail::processEvent(state_, std::move(clientHello)));
  expectActions<MutateState, AttemptVersionFallback>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::Error);

  auto fallback = expectAction<AttemptVersionFallback>(actions);
  std::string expected(
      "\x16\x03\x01\x00\x13"
      "clienthelloencoding",
      24);
  EXPECT_EQ(fallback.clientHello->moveToFbString().toStdString(), expected);
}

TEST_F(ServerProtocolTest, TestClientHelloNoSupportedVersions) {
  setUpExpectingClientHello();
  auto clientHello = TestMessages::clientHello();
  TestMessages::removeExtension(clientHello, ExtensionType::supported_versions);
  auto actions =
      getActions(detail::processEvent(state_, std::move(clientHello)));
  expectError<FizzException>(
      actions,
      AlertDescription::protocol_version,
      "supported version mismatch");
}

TEST_F(ServerProtocolTest, TestClientHelloSupportedVersionsMismatch) {
  setUpExpectingClientHello();
  auto clientHello = TestMessages::clientHello();
  TestMessages::removeExtension(clientHello, ExtensionType::supported_versions);
  SupportedVersions supportedVersions;
  supportedVersions.versions.push_back(static_cast<ProtocolVersion>(0x0200));
  clientHello.extensions.push_back(
      encodeExtension(std::move(supportedVersions)));
  auto actions =
      getActions(detail::processEvent(state_, std::move(clientHello)));
  expectError<FizzException>(
      actions,
      AlertDescription::protocol_version,
      "supported version mismatch");
}

TEST_F(ServerProtocolTest, TestClientHelloCipherMismatch) {
  setUpExpectingClientHello();
  auto clientHello = TestMessages::clientHello();
  clientHello.cipher_suites.clear();
  auto actions =
      getActions(detail::processEvent(state_, std::move(clientHello)));
  expectError<FizzException>(
      actions, AlertDescription::handshake_failure, "no cipher match");
}

TEST_F(ServerProtocolTest, TestClientHelloNoSupportedGroups) {
  setUpExpectingClientHello();
  auto clientHello = TestMessages::clientHello();
  TestMessages::removeExtension(clientHello, ExtensionType::supported_groups);
  auto actions =
      getActions(detail::processEvent(state_, std::move(clientHello)));
  expectError<FizzException>(
      actions, AlertDescription::missing_extension, "no named groups");
}

TEST_F(ServerProtocolTest, TestClientHelloNamedGroupsMismatch) {
  setUpExpectingClientHello();
  auto clientHello = TestMessages::clientHello();
  TestMessages::removeExtension(clientHello, ExtensionType::supported_groups);
  SupportedGroups sg;
  sg.named_group_list.push_back(static_cast<NamedGroup>(0x0707));
  clientHello.extensions.push_back(encodeExtension(std::move(sg)));
  auto actions =
      getActions(detail::processEvent(state_, std::move(clientHello)));
  expectError<FizzException>(
      actions, AlertDescription::handshake_failure, "no group match");
}

TEST_F(ServerProtocolTest, TestClientHelloNoClientKeyShare) {
  setUpExpectingClientHello();
  auto clientHello = TestMessages::clientHello();
  TestMessages::removeExtension(clientHello, ExtensionType::key_share);
  auto actions =
      getActions(detail::processEvent(state_, std::move(clientHello)));
  expectError<FizzException>(
      actions, AlertDescription::missing_extension, "no client share");
}

TEST_F(ServerProtocolTest, TestClientHelloNoSigScemes) {
  setUpExpectingClientHello();
  auto clientHello = TestMessages::clientHello();
  TestMessages::removeExtension(
      clientHello, ExtensionType::signature_algorithms);
  auto actions =
      getActions(detail::processEvent(state_, std::move(clientHello)));
  expectError<FizzException>(
      actions, AlertDescription::missing_extension, "no sig schemes");
}

TEST_F(ServerProtocolTest, TestClientHelloDataAfter) {
  setUpExpectingClientHello();
  EXPECT_CALL(*mockRead_, hasUnparsedHandshakeData())
      .WillRepeatedly(Return(true));
  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHello()));
  expectError<FizzException>(
      actions, AlertDescription::unexpected_message, "data after client hello");
}

TEST_F(ServerProtocolTest, TestClientHelloNoAlpn) {
  setUpExpectingClientHello();
  auto chlo = TestMessages::clientHello();
  TestMessages::removeExtension(
      chlo, ExtensionType::application_layer_protocol_negotiation);
  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_FALSE(state_.alpn().hasValue());
}

TEST_F(ServerProtocolTest, TestClientHelloAlpnMismatch) {
  setUpExpectingClientHello();
  auto chlo = TestMessages::clientHello();
  TestMessages::removeExtension(
      chlo, ExtensionType::application_layer_protocol_negotiation);
  ProtocolNameList alpn;
  ProtocolName gopher;
  gopher.name = folly::IOBuf::copyBuffer("gopher");
  alpn.protocol_name_list.push_back(std::move(gopher));
  chlo.extensions.push_back(encodeExtension(std::move(alpn)));
  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_FALSE(state_.alpn().hasValue());
}

TEST_F(ServerProtocolTest, TestClientHelloServerPref) {
  setUpExpectingClientHello();
  auto chlo = TestMessages::clientHello();
  TestMessages::removeExtension(
      chlo, ExtensionType::application_layer_protocol_negotiation);
  ProtocolNameList alpn;
  ProtocolName gopher;
  gopher.name = folly::IOBuf::copyBuffer("gopher");
  alpn.protocol_name_list.push_back(std::move(gopher));
  ProtocolName h2;
  h2.name = folly::IOBuf::copyBuffer("h2");
  alpn.protocol_name_list.push_back(std::move(h2));
  chlo.extensions.push_back(encodeExtension(std::move(alpn)));
  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(*state_.alpn(), "h2");
}

TEST_F(ServerProtocolTest, TestClientHelloAcceptEarlyData) {
  acceptEarlyData();
  setUpExpectingClientHello();

  std::chrono::milliseconds age =
      std::chrono::minutes(5) - std::chrono::seconds(10);
  auto actions = getActions(detail::processEvent(
      state_, TestMessages::clientHelloPskEarly(age.count())));
  expectActions<
      MutateState,
      WriteToSocket,
      ReportEarlyHandshakeSuccess,
      SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::AcceptingEarlyData);
  EXPECT_EQ(
      *state_.handshakeTime(),
      std::chrono::system_clock::time_point(std::chrono::seconds(10)));
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Accepted);
  EXPECT_EQ(state_.replayCacheResult(), ReplayCacheResult::NotReplay);
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::EarlyData);
  EXPECT_FALSE(state_.appToken().hasValue());
}

TEST_F(ServerProtocolTest, TestClientHelloAcceptEarlyDataOmitEarlyRecort) {
  context_->setOmitEarlyRecordLayer(true);
  acceptEarlyData();
  setUpExpectingClientHello();

  std::chrono::milliseconds age =
      std::chrono::minutes(5) - std::chrono::seconds(10);
  auto actions = getActions(detail::processEvent(
      state_, TestMessages::clientHelloPskEarly(age.count())));
  expectActions<
      MutateState,
      WriteToSocket,
      ReportEarlyHandshakeSuccess,
      SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Accepted);
  EXPECT_EQ(state_.replayCacheResult(), ReplayCacheResult::NotReplay);
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::Handshake);
  EXPECT_EQ(state_.handshakeReadRecordLayer().get(), nullptr);
}

TEST_F(
    ServerProtocolTest,
    TestClientHelloEarlyDataNotAttemptedWithAppTokenValidator) {
  acceptEarlyData();
  setUpExpectingClientHello();
  auto validator = std::make_unique<MockAppTokenValidator>();
  auto validatorPtr = validator.get();
  state_.appTokenValidator() = std::move(validator);

  ON_CALL(*validatorPtr, validate(_)).WillByDefault(InvokeWithoutArgs([]() {
    EXPECT_TRUE(false)
        << "Early data not attempted, validator shoudn't be called";
    return false;
  }));
  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHelloPsk()));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::NotAttempted);
}

TEST_F(ServerProtocolTest, TestClientHelloAcceptEarlyDataWithValidAppToken) {
  acceptEarlyData();
  setUpExpectingClientHello();
  auto validator = std::make_unique<MockAppTokenValidator>();
  auto validatorPtr = validator.get();
  state_.appTokenValidator() = std::move(validator);

  std::string appTokenStr("appToken");

  EXPECT_CALL(*validatorPtr, validate(_))
      .WillOnce(Invoke([&appTokenStr](const ResumptionState& resumptionState) {
        EXPECT_TRUE(IOBufEqualTo()(
            resumptionState.appToken, IOBuf::copyBuffer(appTokenStr)));
        return true;
      }));
  EXPECT_CALL(*mockTicketCipher_, _decrypt(_))
      .WillOnce(InvokeWithoutArgs([=]() {
        ResumptionState res;
        res.version = TestProtocolVersion;
        res.cipher = CipherSuite::TLS_AES_128_GCM_SHA256;
        res.resumptionSecret = folly::IOBuf::copyBuffer("resumesecret");
        res.alpn = "h2";
        res.ticketAgeAdd = 0xffffffff;
        res.ticketIssueTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(10));
        res.appToken = IOBuf::copyBuffer(appTokenStr);
        res.handshakeTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(1));
        return std::make_pair(PskType::Resumption, std::move(res));
      }));

  std::chrono::milliseconds age =
      std::chrono::minutes(5) - std::chrono::seconds(10);
  auto actions = getActions(detail::processEvent(
      state_, TestMessages::clientHelloPskEarly(age.count())));
  expectActions<
      MutateState,
      WriteToSocket,
      ReportEarlyHandshakeSuccess,
      SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::AcceptingEarlyData);
  EXPECT_EQ(
      *state_.handshakeTime(),
      std::chrono::system_clock::time_point(std::chrono::seconds(1)));
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Accepted);
  EXPECT_EQ(state_.replayCacheResult(), ReplayCacheResult::NotReplay);
  EXPECT_TRUE(state_.appToken().hasValue());
  EXPECT_TRUE(
      IOBufEqualTo()(*state_.appToken(), IOBuf::copyBuffer(appTokenStr)));
}

TEST_F(ServerProtocolTest, TestClientHelloRejectEarlyData) {
  setUpExpectingClientHello();

  auto rrl = new MockEncryptedReadRecordLayer(EncryptionLevel::Handshake);
  EXPECT_CALL(*factory_, makeEncryptedReadRecordLayer(_))
      .WillOnce(InvokeWithoutArgs(
          [rrl]() { return std::unique_ptr<EncryptedReadRecordLayer>(rrl); }));
  EXPECT_CALL(*rrl, setSkipFailedDecryption(true));

  auto actions = getActions(
      detail::processEvent(state_, TestMessages::clientHelloPskEarly()));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.readRecordLayer().get(), rrl);
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Rejected);
  EXPECT_FALSE(state_.earlyExporterMasterSecret().hasValue());
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::Handshake);
}

TEST_F(ServerProtocolTest, TestClientHelloHrrRejectEarlyData) {
  acceptEarlyData();
  context_->setSupportedGroups({NamedGroup::secp256r1, NamedGroup::x25519});
  setUpExpectingClientHello();

  auto rrl = new MockPlaintextReadRecordLayer();
  EXPECT_CALL(*factory_, makePlaintextReadRecordLayer())
      .WillOnce(Invoke(
          [rrl]() { return std::unique_ptr<PlaintextReadRecordLayer>(rrl); }));
  EXPECT_CALL(*rrl, setSkipEncryptedRecords(true));

  std::chrono::milliseconds age =
      std::chrono::minutes(5) - std::chrono::seconds(10);
  auto actions = getActions(detail::processEvent(
      state_, TestMessages::clientHelloPskEarly(age.count())));
  expectActions<MutateState, WriteToSocket>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingClientHello);
  EXPECT_EQ(state_.readRecordLayer().get(), rrl);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Rejected);
  EXPECT_FALSE(state_.earlyExporterMasterSecret().hasValue());
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::Plaintext);
  EXPECT_EQ(
      state_.writeRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::Plaintext);
}

TEST_F(ServerProtocolTest, TestClientHelloCookieRejectEarlyData) {
  acceptEarlyData();
  expectCookie();
  setUpExpectingClientHello();

  auto chlo = TestMessages::clientHello();
  chlo.extensions.push_back(encodeExtension(ClientEarlyData()));
  Cookie c;
  c.cookie = IOBuf::copyBuffer("cookie");
  chlo.extensions.push_back(encodeExtension(std::move(c)));
  TestMessages::addPsk(chlo);

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);

  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Rejected);
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::Handshake);
  EXPECT_EQ(
      state_.writeRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::AppTraffic);
}

TEST_F(ServerProtocolTest, TestClientHelloRejectEarlyDataPskRejected) {
  acceptEarlyData();
  setUpExpectingClientHello();

  EXPECT_CALL(*mockTicketCipher_, _decrypt(_)).WillOnce(InvokeWithoutArgs([]() {
    return std::make_pair(PskType::Rejected, none);
  }));

  std::chrono::milliseconds age =
      std::chrono::minutes(5) - std::chrono::seconds(10);
  auto actions = getActions(detail::processEvent(
      state_, TestMessages::clientHelloPskEarly(age.count())));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Rejected);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Rejected);
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::Handshake);
  EXPECT_EQ(
      state_.writeRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::AppTraffic);
}

TEST_F(ServerProtocolTest, TestClientHelloRejectEarlyDataReplayCache) {
  acceptEarlyData();
  setUpExpectingClientHello();

  EXPECT_CALL(*replayCache_, check(_)).WillOnce(InvokeWithoutArgs([] {
    return folly::makeFuture(ReplayCacheResult::DefinitelyReplay);
  }));

  std::chrono::milliseconds age =
      std::chrono::minutes(5) - std::chrono::seconds(10);
  auto actions = getActions(detail::processEvent(
      state_, TestMessages::clientHelloPskEarly(age.count())));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Rejected);
  EXPECT_EQ(state_.replayCacheResult(), ReplayCacheResult::DefinitelyReplay);
}

TEST_F(ServerProtocolTest, TestClientHelloRejectEarlyDataNoAlpn) {
  acceptEarlyData();
  setUpExpectingClientHello();

  std::chrono::milliseconds age =
      std::chrono::minutes(5) - std::chrono::seconds(10);
  auto chlo = TestMessages::clientHelloPskEarly(age.count());
  TestMessages::removeExtension(
      chlo, ExtensionType::application_layer_protocol_negotiation);

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Rejected);
}

TEST_F(ServerProtocolTest, TestClientHelloRejectEarlyDataDiffAlpn) {
  acceptEarlyData();
  setUpExpectingClientHello();

  auto chlo = TestMessages::clientHello();
  TestMessages::removeExtension(
      chlo, ExtensionType::application_layer_protocol_negotiation);
  ProtocolNameList alpn;
  ProtocolName h2;
  h2.name = folly::IOBuf::copyBuffer("h3");
  alpn.protocol_name_list.push_back(std::move(h2));
  chlo.extensions.push_back(encodeExtension(std::move(alpn)));
  chlo.extensions.push_back(encodeExtension(ClientEarlyData()));
  TestMessages::addPsk(chlo);

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Rejected);
}

TEST_F(ServerProtocolTest, TestClientHelloRejectEarlyDataAfterHrr) {
  acceptEarlyData();
  setUpExpectingClientHelloRetry();

  std::chrono::milliseconds age =
      std::chrono::minutes(5) - std::chrono::seconds(10);
  auto actions = getActions(detail::processEvent(
      state_, TestMessages::clientHelloPskEarly(age.count())));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Rejected);
}

TEST_F(ServerProtocolTest, TestClientHelloRejectEarlyDataClockBehind) {
  context_->setEarlyDataSettings(
      true,
      {std::chrono::milliseconds(-1000), std::chrono::milliseconds(1000)},
      replayCache_);
  setUpExpectingClientHello();

  // Actual ticket issued: 10 seconds ago
  mockTicketCipher_->setDefaults(
      std::chrono::system_clock::time_point(std::chrono::seconds(290)));

  auto chlo = TestMessages::clientHello();
  chlo.extensions.push_back(encodeExtension(ClientEarlyData()));
  // Set age here to 4 seconds (6 seconds behind)
  TestMessages::addPsk(chlo, 4000);

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Rejected);
  EXPECT_EQ(*state_.clientClockSkew(), std::chrono::milliseconds(-6000));
}

TEST_F(ServerProtocolTest, TestClientHelloRejectEarlyDataClockAhead) {
  context_->setEarlyDataSettings(
      true,
      {std::chrono::milliseconds(-1000), std::chrono::milliseconds(1000)},
      replayCache_);
  setUpExpectingClientHello();

  // Actual ticket issued: 10 seconds ago
  mockTicketCipher_->setDefaults(
      std::chrono::system_clock::time_point(std::chrono::seconds(290)));

  auto chlo = TestMessages::clientHello();
  chlo.extensions.push_back(encodeExtension(ClientEarlyData()));
  // Client believes issued 16 seconds ago (6 seconds ahead);
  TestMessages::addPsk(chlo, 16000);

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Rejected);
  EXPECT_EQ(*state_.clientClockSkew(), std::chrono::milliseconds(6000));
}

TEST_F(ServerProtocolTest, TestClientHelloRejectEarlyDataTicketAgeOverflow) {
  acceptEarlyData();
  setUpExpectingClientHello();

  EXPECT_CALL(*mockTicketCipher_, _decrypt(_))
      .WillOnce(InvokeWithoutArgs([=]() {
        ResumptionState res;
        res.version = TestProtocolVersion;
        res.cipher = CipherSuite::TLS_AES_128_GCM_SHA256;
        res.resumptionSecret = folly::IOBuf::copyBuffer("resumesecret");
        res.alpn = "h2";
        res.ticketAgeAdd = 0xffffffff;
        res.ticketIssueTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(10));
        res.handshakeTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(1));
        return std::make_pair(PskType::Resumption, std::move(res));
      }));

  auto chlo = TestMessages::clientHello();
  chlo.extensions.push_back(encodeExtension(ClientEarlyData()));
  TestMessages::addPsk(chlo, 2000000);

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Rejected);
  EXPECT_GT(*state_.clientClockSkew(), std::chrono::milliseconds(5000));
}

TEST_F(ServerProtocolTest, TestClientHelloRejectEarlyDataNegativeExpectedAge) {
  acceptEarlyData();
  setUpExpectingClientHello();

  EXPECT_CALL(*mockTicketCipher_, _decrypt(_))
      .WillOnce(InvokeWithoutArgs([=]() {
        ResumptionState res;
        res.version = TestProtocolVersion;
        res.cipher = CipherSuite::TLS_AES_128_GCM_SHA256;
        res.resumptionSecret = folly::IOBuf::copyBuffer("resumesecret");
        res.alpn = "h2";
        res.ticketAgeAdd = 0;
        res.ticketIssueTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(10));
        res.handshakeTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(1));
        return std::make_pair(PskType::Resumption, std::move(res));
      }));

  auto chlo = TestMessages::clientHello();
  chlo.extensions.push_back(encodeExtension(ClientEarlyData()));
  TestMessages::addPsk(chlo, 2000000);

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Rejected);
  EXPECT_GT(*state_.clientClockSkew(), std::chrono::milliseconds(5000));
}

TEST_F(ServerProtocolTest, TestClientHelloRejectEarlyDataInvalidAppToken) {
  acceptEarlyData();
  setUpExpectingClientHello();
  auto validator = std::make_unique<MockAppTokenValidator>();
  auto validatorPtr = validator.get();
  state_.appTokenValidator() = std::move(validator);

  std::string appTokenStr("appToken");

  EXPECT_CALL(*validatorPtr, validate(_))
      .WillOnce(Invoke([&appTokenStr](const ResumptionState& resumptionState) {
        EXPECT_TRUE(IOBufEqualTo()(
            resumptionState.appToken, IOBuf::copyBuffer(appTokenStr)));
        return false;
      }));

  EXPECT_CALL(*mockTicketCipher_, _decrypt(_))
      .WillOnce(InvokeWithoutArgs([=]() {
        ResumptionState res;
        res.version = TestProtocolVersion;
        res.cipher = CipherSuite::TLS_AES_128_GCM_SHA256;
        res.resumptionSecret = folly::IOBuf::copyBuffer("resumesecret");
        res.alpn = "h2";
        res.ticketAgeAdd = 0xffffffff;
        res.ticketIssueTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(10));
        res.appToken = IOBuf::copyBuffer(appTokenStr);
        res.handshakeTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(1));
        return std::make_pair(PskType::Resumption, std::move(res));
      }));

  std::chrono::milliseconds age =
      std::chrono::minutes(5) - std::chrono::seconds(10);
  auto actions = getActions(detail::processEvent(
      state_, TestMessages::clientHelloPskEarly(age.count())));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.earlyDataType(), EarlyDataType::Rejected);
  EXPECT_TRUE(state_.appToken().hasValue());
  EXPECT_TRUE(
      IOBufEqualTo()(*state_.appToken(), IOBuf::copyBuffer(appTokenStr)));
}

TEST_F(ServerProtocolTest, TestClientHelloHandshakeLogging) {
  setUpExpectingClientHello();
  state_.handshakeLogging() = std::make_unique<HandshakeLogging>();
  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHello()));
  processStateMutations(actions);
  EXPECT_EQ(
      state_.handshakeLogging()->clientLegacyVersion, ProtocolVersion::tls_1_2);
  EXPECT_EQ(
      state_.handshakeLogging()->clientSupportedVersions,
      std::vector<ProtocolVersion>({TestProtocolVersion}));
  EXPECT_EQ(
      state_.handshakeLogging()->clientCiphers,
      std::vector<CipherSuite>({CipherSuite::TLS_AES_128_GCM_SHA256,
                                CipherSuite::TLS_AES_256_GCM_SHA384}));
  EXPECT_EQ(
      state_.handshakeLogging()->clientExtensions,
      std::vector<ExtensionType>(
          {ExtensionType::supported_versions,
           ExtensionType::supported_groups,
           ExtensionType::key_share,
           ExtensionType::signature_algorithms,
           ExtensionType::server_name,
           ExtensionType::application_layer_protocol_negotiation,
           ExtensionType::psk_key_exchange_modes}));
  EXPECT_EQ(
      state_.handshakeLogging()->clientSignatureAlgorithms,
      std::vector<SignatureScheme>({SignatureScheme::ecdsa_secp256r1_sha256,
                                    SignatureScheme::rsa_pss_sha256}));
  EXPECT_EQ(*state_.handshakeLogging()->clientSessionIdSent, false);
  EXPECT_TRUE(state_.handshakeLogging()->clientRandom.hasValue());
}

TEST_F(ServerProtocolTest, TestClientHelloHandshakeLoggingError) {
  setUpExpectingClientHello();
  state_.handshakeLogging() = std::make_unique<HandshakeLogging>();
  ClientHello chlo;
  chlo.legacy_version = static_cast<ProtocolVersion>(0x0301);
  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  EXPECT_EQ(
      state_.handshakeLogging()->clientLegacyVersion,
      static_cast<ProtocolVersion>(0x0301));
}

TEST_F(ServerProtocolTest, TestClientHelloNoCompressionMethods) {
  setUpExpectingClientHello();
  auto chlo = TestMessages::clientHello();
  chlo.legacy_compression_methods.clear();
  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectError<FizzException>(
      actions, AlertDescription::illegal_parameter, "compression methods");
}

TEST_F(ServerProtocolTest, TestClientHelloDuplicateExtensions) {
  setUpExpectingClientHello();
  auto chlo = TestMessages::clientHello();
  chlo.extensions.push_back(encodeExtension(SupportedGroups()));
  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectError<FizzException>(
      actions, AlertDescription::illegal_parameter, "duplicate extension");
}

TEST_F(ServerProtocolTest, TestClientHelloDuplicateGroups) {
  setUpExpectingClientHello();
  auto chlo = TestMessages::clientHello();
  TestMessages::removeExtension(chlo, ExtensionType::key_share);

  ClientKeyShare keyShare;
  KeyShareEntry entry1, entry2;

  entry1.group = NamedGroup::x25519;
  entry1.key_exchange = folly::IOBuf::copyBuffer("keyshare");
  keyShare.client_shares.push_back(std::move(entry1));

  entry2.group = NamedGroup::x25519;
  entry2.key_exchange = folly::IOBuf::copyBuffer("keyshare");
  keyShare.client_shares.push_back(std::move(entry2));

  chlo.extensions.push_back(encodeExtension(std::move(keyShare)));

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectError<FizzException>(
      actions,
      AlertDescription::illegal_parameter,
      "duplicate client key share");
}

TEST_F(ServerProtocolTest, TestRetryClientHelloStillNoKeyShare) {
  setUpExpectingClientHelloRetry();
  auto clientHello = TestMessages::clientHello();
  TestMessages::removeExtension(clientHello, ExtensionType::key_share);
  ClientKeyShare keyShare;
  clientHello.extensions.push_back(encodeExtension(std::move(keyShare)));
  auto actions =
      getActions(detail::processEvent(state_, std::move(clientHello)));
  expectError<FizzException>(
      actions, AlertDescription::illegal_parameter, "key share not found");
}

TEST_F(ServerProtocolTest, TestRetryClientHelloCookie) {
  setUpExpectingClientHelloRetry();
  expectCookie();
  auto clientHello = TestMessages::clientHello();
  Cookie c;
  c.cookie = IOBuf::copyBuffer("cookie");
  clientHello.extensions.push_back(encodeExtension(std::move(c)));
  auto actions =
      getActions(detail::processEvent(state_, std::move(clientHello)));
  expectError<FizzException>(
      actions,
      AlertDescription::illegal_parameter,
      "cookie after statefull hrr");
}

TEST_F(ServerProtocolTest, TestRetryClientHelloDifferentVersion) {
  context_->setVersionFallbackEnabled(true);
  setUpExpectingClientHelloRetry();
  auto clientHello = TestMessages::clientHello();
  TestMessages::removeExtension(clientHello, ExtensionType::supported_versions);
  auto actions =
      getActions(detail::processEvent(state_, std::move(clientHello)));
  expectError<FizzException>(
      actions,
      AlertDescription::illegal_parameter,
      "version mismatch with previous negotiation");
}

TEST_F(ServerProtocolTest, TestClientHelloRenegotiatePskCipher) {
  setUpExpectingClientHello();

  EXPECT_CALL(*mockTicketCipher_, _decrypt(_))
      .WillOnce(InvokeWithoutArgs([=]() {
        ResumptionState res;
        res.version = TestProtocolVersion;
        res.cipher = CipherSuite::TLS_CHACHA20_POLY1305_SHA256;
        res.resumptionSecret = folly::IOBuf::copyBuffer("resumesecret");
        res.alpn = "h2";
        res.ticketAgeAdd = 0;
        res.ticketIssueTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(10));
        res.handshakeTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(1));
        return std::make_pair(PskType::Resumption, std::move(res));
      }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHelloPsk()));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Resumption);
  EXPECT_EQ(state_.cipher(), CipherSuite::TLS_AES_128_GCM_SHA256);
}

TEST_F(ServerProtocolTest, TestClientHelloRenegotiatePskCipherIncompatible) {
  setUpExpectingClientHello();

  EXPECT_CALL(*mockTicketCipher_, _decrypt(_))
      .WillOnce(InvokeWithoutArgs([=]() {
        ResumptionState res;
        res.version = TestProtocolVersion;
        res.cipher = CipherSuite::TLS_AES_256_GCM_SHA384;
        res.resumptionSecret = folly::IOBuf::copyBuffer("resumesecret");
        res.alpn = "h2";
        res.ticketAgeAdd = 0;
        res.ticketIssueTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(10));
        res.handshakeTime =
            std::chrono::system_clock::time_point(std::chrono::seconds(1));
        return std::make_pair(PskType::Resumption, std::move(res));
      }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::clientHelloPsk()));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.pskType(), PskType::Rejected);
  EXPECT_EQ(state_.cipher(), CipherSuite::TLS_AES_128_GCM_SHA256);
}

TEST_F(ServerProtocolTest, TestClientHelloCookie) {
  expectCookie();
  setUpExpectingClientHello();

  auto chlo = TestMessages::clientHello();
  Cookie c;
  c.cookie = IOBuf::copyBuffer("cookie");
  chlo.extensions.push_back(encodeExtension(std::move(c)));

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.cipher(), CipherSuite::TLS_AES_128_GCM_SHA256);
}

TEST_F(ServerProtocolTest, TestClientHelloCookieFail) {
  expectCookie();
  setUpExpectingClientHello();

  auto chlo = TestMessages::clientHello();
  Cookie c;
  c.cookie = IOBuf::copyBuffer("xyz");
  chlo.extensions.push_back(encodeExtension(std::move(c)));

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectError<FizzException>(
      actions, AlertDescription::decrypt_error, "decrypt cookie");
}

TEST_F(ServerProtocolTest, TestClientHelloCookieNoCipher) {
  setUpExpectingClientHello();

  auto chlo = TestMessages::clientHello();
  Cookie c;
  c.cookie = IOBuf::copyBuffer("cookie");
  chlo.extensions.push_back(encodeExtension(std::move(c)));

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectError<FizzException>(
      actions, AlertDescription::unsupported_extension, "no cookie cipher");
}

TEST_F(ServerProtocolTest, TestClientHelloCookieVersionMismatch) {
  setUpExpectingClientHello();
  acceptCookies();

  EXPECT_CALL(*mockCookieCipher_, _decrypt(_)).WillOnce(Invoke([](Buf& cookie) {
    EXPECT_TRUE(IOBufEqualTo()(cookie, IOBuf::copyBuffer("cookie")));
    CookieState cs;
    cs.version = ProtocolVersion::tls_1_2;
    cs.cipher = CipherSuite::TLS_AES_128_GCM_SHA256;
    cs.chloHash = IOBuf::copyBuffer("chlohash");
    cs.appToken = IOBuf::create(0);
    return folly::Optional<CookieState>(std::move(cs));
  }));

  auto chlo = TestMessages::clientHello();
  Cookie c;
  c.cookie = IOBuf::copyBuffer("cookie");
  chlo.extensions.push_back(encodeExtension(std::move(c)));

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectError<FizzException>(
      actions,
      AlertDescription::protocol_version,
      "version mismatch with cookie");
}

TEST_F(ServerProtocolTest, TestClientHelloCookieCipherMismatch) {
  setUpExpectingClientHello();
  acceptCookies();

  EXPECT_CALL(*mockCookieCipher_, _decrypt(_)).WillOnce(Invoke([](Buf& cookie) {
    EXPECT_TRUE(IOBufEqualTo()(cookie, IOBuf::copyBuffer("cookie")));
    CookieState cs;
    cs.version = TestProtocolVersion;
    cs.cipher = CipherSuite::TLS_AES_256_GCM_SHA384;
    cs.chloHash = IOBuf::copyBuffer("chlohash");
    cs.appToken = IOBuf::create(0);
    return folly::Optional<CookieState>(std::move(cs));
  }));

  auto chlo = TestMessages::clientHello();
  Cookie c;
  c.cookie = IOBuf::copyBuffer("cookie");
  chlo.extensions.push_back(encodeExtension(std::move(c)));

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectError<FizzException>(
      actions,
      AlertDescription::handshake_failure,
      "cipher mismatch with cookie");
}

TEST_F(ServerProtocolTest, TestClientHelloCookieGroupMismatch) {
  setUpExpectingClientHello();
  acceptCookies();

  EXPECT_CALL(*mockCookieCipher_, _decrypt(_)).WillOnce(Invoke([](Buf& cookie) {
    EXPECT_TRUE(IOBufEqualTo()(cookie, IOBuf::copyBuffer("cookie")));
    CookieState cs;
    cs.version = TestProtocolVersion;
    cs.cipher = CipherSuite::TLS_AES_128_GCM_SHA256;
    cs.group = NamedGroup::secp256r1;
    cs.chloHash = IOBuf::copyBuffer("chlohash");
    cs.appToken = IOBuf::create(0);
    return folly::Optional<CookieState>(std::move(cs));
  }));

  auto chlo = TestMessages::clientHello();
  Cookie c;
  c.cookie = IOBuf::copyBuffer("cookie");
  chlo.extensions.push_back(encodeExtension(std::move(c)));

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectError<FizzException>(
      actions,
      AlertDescription::illegal_parameter,
      "group mismatch with cookie");
}

TEST_F(ServerProtocolTest, TestClientHelloCookieNoGroup) {
  expectCookie();
  setUpExpectingClientHello();

  auto chlo = TestMessages::clientHello();
  Cookie c;
  c.cookie = IOBuf::copyBuffer("cookie");
  chlo.extensions.push_back(encodeExtension(std::move(c)));
  TestMessages::removeExtension(chlo, ExtensionType::key_share);
  ClientKeyShare keyShare;
  chlo.extensions.push_back(encodeExtension(std::move(keyShare)));

  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectError<FizzException>(
      actions, AlertDescription::illegal_parameter, "key share not found");
}

TEST_F(ServerProtocolTest, TestNoCertCompressionAlgorithmMatch) {
  setUpExpectingClientHello();
  context_->setSupportedCompressionAlgorithms(
      {CertificateCompressionAlgorithm::zlib});
  auto chlo = TestMessages::clientHello();
  CertificateCompressionAlgorithms algos;
  algos.algorithms = {static_cast<CertificateCompressionAlgorithm>(0xfb)};
  chlo.extensions.push_back(encodeExtension(algos));
  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.serverCertCompAlgo(), folly::none);
}

TEST_F(ServerProtocolTest, TestCertCompressionRequestedNotSupported) {
  setUpExpectingClientHello();
  auto chlo = TestMessages::clientHello();
  CertificateCompressionAlgorithms algos;
  algos.algorithms = {static_cast<CertificateCompressionAlgorithm>(0xfb)};
  chlo.extensions.push_back(encodeExtension(algos));
  auto actions = getActions(detail::processEvent(state_, std::move(chlo)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.serverCertCompAlgo(), folly::none);
}

TEST_F(ServerProtocolTest, TestEarlyAppData) {
  setUpAcceptingEarlyData();

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::appData()));

  expectSingleAction<DeliverAppData>(std::move(actions));
}

TEST_F(ServerProtocolTest, TestEarlyAppWrite) {
  setUpAcceptingEarlyData();
  EXPECT_CALL(*mockWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = mockWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::application_data);
    EXPECT_TRUE(IOBufEqualTo()(msg.fragment, IOBuf::copyBuffer("appdata")));
    content.data = IOBuf::copyBuffer("writtenappdata");
    return content;
  }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::appWrite()));

  auto write = expectSingleAction<WriteToSocket>(std::move(actions));
  EXPECT_TRUE(IOBufEqualTo()(
      write.contents[0].data, IOBuf::copyBuffer("writtenappdata")));
}

TEST_F(ServerProtocolTest, TestEndOfEarlyData) {
  setUpAcceptingEarlyData();
  auto handshakeReadRecordLayer = state_.handshakeReadRecordLayer().get();

  EXPECT_CALL(
      *mockHandshakeContext_, appendToTranscript(BufMatches("eoedencoding")));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::endOfEarlyData()));
  expectActions<MutateState>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
  EXPECT_EQ(state_.readRecordLayer().get(), handshakeReadRecordLayer);
  EXPECT_EQ(state_.writeRecordLayer().get(), mockWrite_);
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::Handshake);
}

TEST_F(ServerProtocolTest, TestEndOfEarlyDataExtraData) {
  setUpAcceptingEarlyData();

  EXPECT_CALL(*mockRead_, hasUnparsedHandshakeData())
      .WillRepeatedly(Return(true));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::endOfEarlyData()));
  expectError<FizzException>(
      actions, AlertDescription::unexpected_message, "data after eoed");
}

TEST_F(ServerProtocolTest, TestFullHandshakeFinished) {
  setUpExpectingFinished();
  Sequence contextSeq;
  EXPECT_CALL(
      *mockHandshakeContext_, getFinishedData(RangeMatches("clihandsec")))
      .InSequence(contextSeq)
      .WillOnce(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("verifydata"); }));
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(
          Invoke([]() { return IOBuf::copyBuffer("sfincontext"); }));
  EXPECT_CALL(
      *mockHandshakeContext_,
      appendToTranscript(BufMatches("finishedencoding")))
      .InSequence(contextSeq);
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(
          Invoke([]() { return IOBuf::copyBuffer("clifincontext"); }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getSecret(MasterSecrets::ResumptionMaster, RangeMatches("clifincontext")))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'r', 's', 'e', 'c'}),
            MasterSecrets::ResumptionMaster);
      }));
  EXPECT_CALL(
      *mockKeyScheduler_, getSecret(AppTrafficSecrets::ClientAppTraffic))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'c', 'a', 't'}),
            AppTrafficSecrets::ClientAppTraffic);
      }));
  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("cat"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("clientkey"),
                          IOBuf::copyBuffer("clientiv")};
      }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getResumptionSecret(RangeMatches("rsec"), RangeMatches("")))
      .WillOnce(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("derivedrsec"); }));

  MockAead* raead;
  MockEncryptedReadRecordLayer* rrl;
  expectAeadCreation(&raead, nullptr);
  expectEncryptedReadRecordLayerCreation(&rrl, &raead, StringPiece("cat"));
  EXPECT_CALL(*factory_, makeTicketAgeAdd()).WillOnce(Return(0x44444444));
  EXPECT_CALL(*mockTicketCipher_, _encrypt(_))
      .WillOnce(Invoke([=](ResumptionState& resState) {
        EXPECT_EQ(resState.version, TestProtocolVersion);
        EXPECT_EQ(resState.cipher, CipherSuite::TLS_AES_128_GCM_SHA256);
        EXPECT_TRUE(IOBufEqualTo()(
            resState.resumptionSecret, IOBuf::copyBuffer("derivedrsec")));
        EXPECT_EQ(resState.serverCert, cert_);
        EXPECT_EQ(*resState.alpn, "h2");
        EXPECT_EQ(resState.ticketAgeAdd, 0x44444444);
        return std::make_pair(
            IOBuf::copyBuffer("ticket"), std::chrono::seconds(100));
      }));
  EXPECT_CALL(*mockWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = mockWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::handshake);
    EXPECT_TRUE(IOBufEqualTo()(
        msg.fragment, encodeHandshake(TestMessages::newSessionTicket())));
    content.data = folly::IOBuf::copyBuffer("handshake");
    return content;
  }));
  EXPECT_CALL(*mockKeyScheduler_, clearMasterSecret());

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::finished()));

  expectActions<
      MutateState,
      ReportHandshakeSuccess,
      WriteToSocket,
      SecretAvailable>(actions);

  expectSecret(
      actions, AppTrafficSecrets::ClientAppTraffic, StringPiece("cat"));
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::AcceptingData);
  EXPECT_EQ(state_.readRecordLayer().get(), rrl);
  EXPECT_EQ(
      state_.readRecordLayer()->getEncryptionLevel(),
      EncryptionLevel::AppTraffic);
  EXPECT_EQ(state_.writeRecordLayer().get(), mockWrite_);
  EXPECT_TRUE(state_.handshakeTime().hasValue());
  EXPECT_EQ(
      *state_.handshakeTime(),
      std::chrono::system_clock::time_point(std::chrono::seconds(10)));
  ASSERT_THAT(state_.resumptionMasterSecret(), ElementsAre('r', 's', 'e', 'c'));
}

TEST_F(ServerProtocolTest, TestFinishedNoTicket) {
  setUpExpectingFinished();
  EXPECT_CALL(*mockTicketCipher_, _encrypt(_)).WillOnce(InvokeWithoutArgs([]() {
    return none;
  }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::finished()));
  expectActions<MutateState, ReportHandshakeSuccess, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::AcceptingData);
}

TEST_F(ServerProtocolTest, TestFinishedTicketEarly) {
  acceptEarlyData();
  setUpExpectingFinished();

  EXPECT_CALL(*factory_, makeTicketAgeAdd()).WillOnce(Return(0x44444444));
  EXPECT_CALL(*mockWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = mockWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::handshake);
    auto nst = TestMessages::newSessionTicket();
    TicketEarlyData early;
    early.max_early_data_size = 0xffffffff;
    nst.extensions.push_back(encodeExtension(std::move(early)));
    EXPECT_TRUE(IOBufEqualTo()(msg.fragment, encodeHandshake(std::move(nst))));
    content.data = folly::IOBuf::copyBuffer("handshake");
    return content;
  }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::finished()));
  expectActions<
      MutateState,
      ReportHandshakeSuccess,
      WriteToSocket,
      SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::AcceptingData);
}

TEST_F(ServerProtocolTest, TestFinishedPskNotSupported) {
  setUpExpectingFinished();
  state_.pskType() = PskType::NotSupported;

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::finished()));
  expectActions<MutateState, ReportHandshakeSuccess, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::AcceptingData);
}

TEST_F(ServerProtocolTest, TestFinishedNoAutomaticNewSessionTicket) {
  setUpExpectingFinished();
  context_->setSendNewSessionTicket(false);

  EXPECT_CALL(*mockKeyScheduler_, clearMasterSecret());
  auto actions =
      getActions(detail::processEvent(state_, TestMessages::finished()));
  expectActions<MutateState, ReportHandshakeSuccess, SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::AcceptingData);
}

TEST_F(ServerProtocolTest, TestFinishedMismatch) {
  setUpExpectingFinished();
  EXPECT_CALL(
      *mockHandshakeContext_, getFinishedData(RangeMatches("clihandsec")))
      .WillOnce(InvokeWithoutArgs(
          []() { return IOBuf::copyBuffer("wrongverifydata"); }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::finished()));

  expectError<FizzException>(actions, none, "finished verify failure");
}

TEST_F(ServerProtocolTest, TestFinishedExtraData) {
  setUpExpectingFinished();
  EXPECT_CALL(*mockRead_, hasUnparsedHandshakeData())
      .WillRepeatedly(Return(true));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::finished()));

  expectError<FizzException>(actions, none, "data after finished");
}

TEST_F(ServerProtocolTest, TestExpectingFinishedAppWrite) {
  setUpExpectingFinished();
  EXPECT_CALL(*mockWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = mockWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::application_data);
    EXPECT_TRUE(IOBufEqualTo()(msg.fragment, IOBuf::copyBuffer("appdata")));
    content.data = IOBuf::copyBuffer("writtenappdata");
    return content;
  }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::appWrite()));

  auto write = expectSingleAction<WriteToSocket>(std::move(actions));
  EXPECT_TRUE(IOBufEqualTo()(
      write.contents[0].data, IOBuf::copyBuffer("writtenappdata")));
}

TEST_F(ServerProtocolTest, TestWriteNewSessionTicket) {
  setUpAcceptingData();
  context_->setSendNewSessionTicket(false);
  state_.resumptionMasterSecret() = std::vector<uint8_t>({'r', 's', 'e', 'c'});

  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .WillRepeatedly(
          Invoke([]() { return IOBuf::copyBuffer("clifincontext"); }));
  EXPECT_CALL(
      *mockKeyScheduler_,
      getResumptionSecret(RangeMatches("rsec"), RangeMatches("")))
      .WillOnce(
          InvokeWithoutArgs([]() { return IOBuf::copyBuffer("derivedrsec"); }));

  EXPECT_CALL(*factory_, makeTicketAgeAdd()).WillOnce(Return(0x44444444));
  EXPECT_CALL(*mockTicketCipher_, _encrypt(_))
      .WillOnce(Invoke([=](ResumptionState& resState) {
        EXPECT_EQ(resState.version, TestProtocolVersion);
        EXPECT_EQ(resState.cipher, CipherSuite::TLS_AES_128_GCM_SHA256);
        EXPECT_TRUE(IOBufEqualTo()(
            resState.resumptionSecret, IOBuf::copyBuffer("derivedrsec")));
        EXPECT_EQ(resState.serverCert, cert_);
        EXPECT_EQ(*resState.alpn, "h2");
        EXPECT_EQ(resState.ticketAgeAdd, 0x44444444);
        return std::make_pair(
            IOBuf::copyBuffer("ticket"), std::chrono::seconds(100));
      }));
  auto nstBuf = folly::IOBuf::copyBuffer("nst");
  EXPECT_CALL(*appWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = appWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::handshake);
    EXPECT_TRUE(IOBufEqualTo()(
        msg.fragment, encodeHandshake(TestMessages::newSessionTicket())));
    content.data = nstBuf->clone();
    return content;
  }));

  auto actions =
      getActions(detail::processEvent(state_, WriteNewSessionTicket()));
  auto write = expectSingleAction<WriteToSocket>(std::move(actions));
  EXPECT_EQ(write.contents[0].contentType, ContentType::handshake);
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::AppTraffic);
  EXPECT_TRUE(IOBufEqualTo()(nstBuf, write.contents[0].data));
}

TEST_F(ServerProtocolTest, TestWriteNewSessionTicketWithTicketEarly) {
  acceptEarlyData();
  setUpAcceptingData();
  context_->setSendNewSessionTicket(false);

  EXPECT_CALL(*appWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = appWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::handshake);
    auto nst = TestMessages::newSessionTicket();
    TicketEarlyData early;
    early.max_early_data_size = 0xffffffff;
    nst.extensions.push_back(encodeExtension(std::move(early)));
    EXPECT_TRUE(IOBufEqualTo()(msg.fragment, encodeHandshake(std::move(nst))));
    content.data = folly::IOBuf::copyBuffer("handshake");
    return content;
  }));

  auto actions =
      getActions(detail::processEvent(state_, WriteNewSessionTicket()));
  expectSingleAction<WriteToSocket>(std::move(actions));
}

TEST_F(ServerProtocolTest, TestWriteNewSessionTicketWithAppToken) {
  setUpAcceptingData();
  context_->setSendNewSessionTicket(false);

  std::string appToken("appToken");

  EXPECT_CALL(*factory_, makeTicketAgeAdd()).WillOnce(Return(0x44444444));
  EXPECT_CALL(*mockTicketCipher_, _encrypt(_))
      .WillOnce(Invoke([=](ResumptionState& resState) {
        EXPECT_EQ(resState.serverCert, cert_);
        EXPECT_EQ(*resState.alpn, "h2");
        EXPECT_EQ(resState.ticketAgeAdd, 0x44444444);
        EXPECT_TRUE(
            IOBufEqualTo()(resState.appToken, IOBuf::copyBuffer(appToken)));
        return std::make_pair(
            IOBuf::copyBuffer("ticket"), std::chrono::seconds(100));
      }));

  WriteNewSessionTicket writeNewSessionTicket;
  writeNewSessionTicket.appToken = IOBuf::copyBuffer(appToken);
  auto actions = getActions(
      detail::processEvent(state_, std::move(writeNewSessionTicket)));
  expectSingleAction<WriteToSocket>(std::move(actions));
}

TEST_F(
    ServerProtocolTest,
    TestWriteNewSessionTicketWithAppTokenAfterAutomaticSend) {
  setUpExpectingFinished();
  context_->setSendNewSessionTicket(true);

  // ExpectingFinished -> AcceptingData
  EXPECT_CALL(*factory_, makeTicketAgeAdd()).WillOnce(Return(0x44444444));
  EXPECT_CALL(*mockTicketCipher_, _encrypt(_))
      .WillOnce(Invoke([=](ResumptionState& resState) {
        EXPECT_EQ(resState.serverCert, cert_);
        EXPECT_EQ(*resState.alpn, "h2");
        EXPECT_EQ(resState.ticketAgeAdd, 0x44444444);
        EXPECT_FALSE(resState.appToken);
        return std::make_pair(
            IOBuf::copyBuffer("ticket"), std::chrono::seconds(100));
      }));
  auto actions =
      getActions(detail::processEvent(state_, TestMessages::finished()));
  expectActions<
      MutateState,
      ReportHandshakeSuccess,
      WriteToSocket,
      SecretAvailable>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.state(), StateEnum::AcceptingData);

  // AcceptingData: WriteNewSessionTicket
  std::string appToken("appToken");
  EXPECT_CALL(*factory_, makeTicketAgeAdd()).WillOnce(Return(0x44444444));
  EXPECT_CALL(*mockTicketCipher_, _encrypt(_))
      .WillOnce(Invoke([=](ResumptionState& resState) {
        EXPECT_EQ(resState.serverCert, cert_);
        EXPECT_EQ(*resState.alpn, "h2");
        EXPECT_EQ(resState.ticketAgeAdd, 0x44444444);
        EXPECT_TRUE(
            IOBufEqualTo()(resState.appToken, IOBuf::copyBuffer(appToken)));
        return std::make_pair(
            IOBuf::copyBuffer("ticket"), std::chrono::seconds(100));
      }));
  WriteNewSessionTicket writeNewSessionTicket;
  writeNewSessionTicket.appToken = IOBuf::copyBuffer(appToken);
  auto writeNewSessionTicketActions = getActions(
      detail::processEvent(state_, std::move(writeNewSessionTicket)));
  expectSingleAction<WriteToSocket>(std::move(writeNewSessionTicketActions));
}

TEST_F(ServerProtocolTest, TestWriteNewSessionTicketNoTicket) {
  setUpAcceptingData();
  context_->setSendNewSessionTicket(false);

  EXPECT_CALL(*mockTicketCipher_, _encrypt(_)).WillOnce(InvokeWithoutArgs([]() {
    return none;
  }));

  auto actions =
      getActions(detail::processEvent(state_, WriteNewSessionTicket()));
  EXPECT_TRUE(actions.empty());
}

TEST_F(ServerProtocolTest, TestWriteNewSessionTicketPskNotSupported) {
  setUpAcceptingData();
  context_->setSendNewSessionTicket(false);
  state_.pskType() = PskType::NotSupported;

  auto actions =
      getActions(detail::processEvent(state_, WriteNewSessionTicket()));
  EXPECT_TRUE(actions.empty());
}

TEST_F(ServerProtocolTest, TestAppData) {
  setUpAcceptingData();

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::appData()));

  expectSingleAction<DeliverAppData>(std::move(actions));
}

TEST_F(ServerProtocolTest, TestAppWrite) {
  setUpAcceptingData();
  EXPECT_CALL(*appWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = appWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::application_data);
    EXPECT_TRUE(IOBufEqualTo()(msg.fragment, IOBuf::copyBuffer("appdata")));
    content.data = IOBuf::copyBuffer("writtenappdata");
    return content;
  }));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::appWrite()));

  auto write = expectSingleAction<WriteToSocket>(std::move(actions));
  EXPECT_TRUE(IOBufEqualTo()(
      write.contents[0].data, IOBuf::copyBuffer("writtenappdata")));
  EXPECT_EQ(write.contents[0].encryptionLevel, EncryptionLevel::AppTraffic);
  EXPECT_EQ(write.contents[0].contentType, ContentType::application_data);
}

TEST_F(ServerProtocolTest, TestKeyUpdateNotRequested) {
  setUpAcceptingData();
  auto actions =
      getActions(detail::processEvent(state_, TestMessages::keyUpdate(false)));
  expectActions<MutateState>(actions);
  EXPECT_EQ(getNumActions<WriteToSocket>(actions, false), 0);
}

TEST_F(ServerProtocolTest, TestKeyUpdateExtraData) {
  setUpAcceptingData();
  EXPECT_CALL(*appRead_, hasUnparsedHandshakeData())
      .WillRepeatedly(Return(true));
  auto actions =
      getActions(detail::processEvent(state_, TestMessages::keyUpdate(false)));

  expectError<FizzException>(actions, none, "data after key_update");
}

TEST_F(ServerProtocolTest, TestKeyUpdateRequest) {
  setUpAcceptingData();
  EXPECT_CALL(*mockKeyScheduler_, clientKeyUpdate());
  EXPECT_CALL(*appRead_, hasUnparsedHandshakeData()).WillOnce(Return(false));

  EXPECT_CALL(
      *mockKeyScheduler_, getSecret(AppTrafficSecrets::ClientAppTraffic))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'c', 'a', 't'}),
            AppTrafficSecrets::ClientAppTraffic);
      }));

  EXPECT_CALL(*appWrite_, _write(_)).WillOnce(Invoke([&](TLSMessage& msg) {
    TLSContent content;
    content.contentType = msg.type;
    content.encryptionLevel = appWrite_->getEncryptionLevel();
    EXPECT_EQ(msg.type, ContentType::handshake);
    EXPECT_TRUE(IOBufEqualTo()(
        msg.fragment, encodeHandshake(TestMessages::keyUpdate(false))));
    content.data = folly::IOBuf::copyBuffer("keyupdated");
    return content;
  }));

  EXPECT_CALL(*mockKeyScheduler_, serverKeyUpdate());
  EXPECT_CALL(
      *mockKeyScheduler_, getSecret(AppTrafficSecrets::ServerAppTraffic))
      .WillOnce(InvokeWithoutArgs([]() {
        return DerivedSecret(
            std::vector<uint8_t>({'s', 'a', 't'}),
            AppTrafficSecrets::ServerAppTraffic);
      }));

  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("cat"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("clientkey"),
                          IOBuf::copyBuffer("clientiv")};
      }));

  EXPECT_CALL(*mockKeyScheduler_, getTrafficKey(RangeMatches("sat"), _, _))
      .WillOnce(InvokeWithoutArgs([]() {
        return TrafficKey{IOBuf::copyBuffer("serverkey"),
                          IOBuf::copyBuffer("serveriv")};
      }));

  MockAead* raead;
  MockAead* waead;
  MockEncryptedReadRecordLayer* rrl;
  MockEncryptedWriteRecordLayer* wrl;

  expectAeadCreation(&raead, &waead);
  expectEncryptedReadRecordLayerCreation(&rrl, &raead, StringPiece("cat"));
  expectEncryptedWriteRecordLayerCreation(&wrl, &waead, StringPiece("sat"));
  auto actions =
      getActions(detail::processEvent(state_, TestMessages::keyUpdate(true)));
  expectActions<MutateState, WriteToSocket, SecretAvailable>(actions);
  auto write = expectAction<WriteToSocket>(actions);
  EXPECT_TRUE(
      IOBufEqualTo()(write.contents[0].data, IOBuf::copyBuffer("keyupdated")));

  expectSecret(
      actions, AppTrafficSecrets::ClientAppTraffic, StringPiece("cat"));
  expectSecret(
      actions, AppTrafficSecrets::ServerAppTraffic, StringPiece("sat"));
  processStateMutations(actions);
  EXPECT_EQ(state_.readRecordLayer().get(), rrl);
  EXPECT_EQ(state_.writeRecordLayer().get(), wrl);
  EXPECT_EQ(state_.state(), StateEnum::AcceptingData);
}

TEST_F(ServerProtocolTest, TestCertificate) {
  setUpExpectingCertificate();
  EXPECT_CALL(
      *mockHandshakeContext_, appendToTranscript(BufMatches("certencoding")));
  clientLeafCert_ = std::make_shared<MockPeerCert>();
  clientIntCert_ = std::make_shared<MockPeerCert>();
  EXPECT_CALL(*factory_, _makePeerCert(BufMatches("cert1")))
      .WillOnce(Return(clientLeafCert_));
  EXPECT_CALL(*factory_, _makePeerCert(BufMatches("cert2")))
      .WillOnce(Return(clientIntCert_));

  auto certificate = TestMessages::certificate();
  CertificateEntry entry1;
  entry1.cert_data = folly::IOBuf::copyBuffer("cert1");
  certificate.certificate_list.push_back(std::move(entry1));
  CertificateEntry entry2;
  entry2.cert_data = folly::IOBuf::copyBuffer("cert2");
  certificate.certificate_list.push_back(std::move(entry2));
  auto actions =
      getActions(detail::processEvent(state_, std::move(certificate)));

  expectActions<MutateState>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.unverifiedCertChain()->size(), 2);
  EXPECT_EQ(state_.unverifiedCertChain()->at(0), clientLeafCert_);
  EXPECT_EQ(state_.unverifiedCertChain()->at(1), clientIntCert_);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingCertificateVerify);
}

TEST_F(ServerProtocolTest, TestCertificateNonemptyContext) {
  setUpExpectingCertificate();
  EXPECT_CALL(
      *mockHandshakeContext_, appendToTranscript(BufMatches("certencoding")));

  auto badCertMsg = TestMessages::certificate();
  badCertMsg.certificate_request_context =
      folly::IOBuf::copyBuffer("garbagecontext");
  auto actions =
      getActions(detail::processEvent(state_, std::move(badCertMsg)));

  expectError<FizzException>(
      actions,
      AlertDescription::illegal_parameter,
      "certificate request context must be empty");
}

TEST_F(ServerProtocolTest, TestCertificateEmptyForbidden) {
  setUpExpectingCertificate();
  EXPECT_CALL(
      *mockHandshakeContext_, appendToTranscript(BufMatches("certencoding")));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::certificate()));

  expectError<FizzException>(
      actions,
      AlertDescription::certificate_required,
      "certificate requested but none received");
}

TEST_F(ServerProtocolTest, TestCertificateEmptyPermitted) {
  setUpExpectingCertificate();
  context_->setClientAuthMode(ClientAuthMode::Optional);
  EXPECT_CALL(
      *mockHandshakeContext_, appendToTranscript(BufMatches("certencoding")));

  auto actions =
      getActions(detail::processEvent(state_, TestMessages::certificate()));

  expectActions<MutateState>(actions);
  processStateMutations(actions);
  EXPECT_FALSE(state_.unverifiedCertChain().hasValue());
  EXPECT_EQ(state_.clientCert(), nullptr);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
}

TEST_F(ServerProtocolTest, TestCertificateVerifyNoVerifier) {
  setUpExpectingCertificateVerify();
  context_->setClientCertVerifier(nullptr);
  Sequence contextSeq;
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(
          Invoke([]() { return IOBuf::copyBuffer("certcontext"); }));

  EXPECT_CALL(
      *clientLeafCert_,
      verify(
          SignatureScheme::ecdsa_secp256r1_sha256,
          CertificateVerifyContext::Client,
          RangeMatches("certcontext"),
          RangeMatches("signature")))
      .InSequence(contextSeq);

  EXPECT_CALL(
      *mockHandshakeContext_,
      appendToTranscript(BufMatches("certverifyencoding")))
      .InSequence(contextSeq);

  auto actions = getActions(
      detail::processEvent(state_, TestMessages::certificateVerify()));

  expectActions<MutateState>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.unverifiedCertChain(), folly::none);
  EXPECT_EQ(state_.clientCert(), clientLeafCert_);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
}

TEST_F(ServerProtocolTest, TestCertificateVerifyWithVerifier) {
  setUpExpectingCertificateVerify();
  Sequence contextSeq;
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(
          Invoke([]() { return IOBuf::copyBuffer("certcontext"); }));

  EXPECT_CALL(
      *clientLeafCert_,
      verify(
          SignatureScheme::ecdsa_secp256r1_sha256,
          CertificateVerifyContext::Client,
          RangeMatches("certcontext"),
          RangeMatches("signature")))
      .InSequence(contextSeq);

  EXPECT_CALL(*certVerifier_, verify(_))
      .InSequence(contextSeq)
      .WillOnce(Invoke(
          [this](const std::vector<std::shared_ptr<const PeerCert>>& certs) {
            EXPECT_EQ(certs.size(), 2);
            EXPECT_EQ(certs[0], clientLeafCert_);
            EXPECT_EQ(certs[1], clientIntCert_);
          }));

  EXPECT_CALL(
      *mockHandshakeContext_,
      appendToTranscript(BufMatches("certverifyencoding")))
      .InSequence(contextSeq);

  auto actions = getActions(
      detail::processEvent(state_, TestMessages::certificateVerify()));

  expectActions<MutateState>(actions);
  processStateMutations(actions);
  EXPECT_EQ(state_.unverifiedCertChain(), folly::none);
  EXPECT_EQ(state_.clientCert(), clientLeafCert_);
  EXPECT_EQ(state_.state(), StateEnum::ExpectingFinished);
}

TEST_F(ServerProtocolTest, TestCertificateVerifyAlgoMismatch) {
  setUpExpectingCertificateVerify();

  context_->setSupportedSigSchemes({SignatureScheme::ed25519});

  auto actions = getActions(
      detail::processEvent(state_, TestMessages::certificateVerify()));

  expectError<FizzException>(
      actions,
      AlertDescription::handshake_failure,
      "client chose unsupported sig scheme:");
}

TEST_F(ServerProtocolTest, TestCertificateVerifySignatureFailure) {
  setUpExpectingCertificateVerify();
  Sequence contextSeq;
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(
          Invoke([]() { return IOBuf::copyBuffer("certcontext"); }));

  EXPECT_CALL(
      *clientLeafCert_,
      verify(
          SignatureScheme::ecdsa_secp256r1_sha256,
          CertificateVerifyContext::Client,
          RangeMatches("certcontext"),
          RangeMatches("signature")))
      .InSequence(contextSeq)
      .WillOnce(Throw(
          FizzException("verify failed", AlertDescription::bad_record_mac)));

  auto actions = getActions(
      detail::processEvent(state_, TestMessages::certificateVerify()));

  expectError<FizzException>(
      actions, AlertDescription::bad_record_mac, "verify failed");
}

TEST_F(ServerProtocolTest, TestCertificateVerifyVerifierFailure) {
  setUpExpectingCertificateVerify();
  Sequence contextSeq;
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(
          Invoke([]() { return IOBuf::copyBuffer("certcontext"); }));

  EXPECT_CALL(
      *clientLeafCert_,
      verify(
          SignatureScheme::ecdsa_secp256r1_sha256,
          CertificateVerifyContext::Client,
          RangeMatches("certcontext"),
          RangeMatches("signature")))
      .InSequence(contextSeq);

  EXPECT_CALL(*certVerifier_, verify(_))
      .InSequence(contextSeq)
      .WillOnce(Throw(FizzVerificationException(
          "verifier failed", AlertDescription::bad_certificate)));

  auto actions = getActions(
      detail::processEvent(state_, TestMessages::certificateVerify()));

  expectError<FizzVerificationException>(
      actions, AlertDescription::bad_certificate, "verifier failed");
}

TEST_F(ServerProtocolTest, TestOptionalCertificateVerifySignatureFailure) {
  setUpExpectingCertificateVerify();
  context_->setClientAuthMode(ClientAuthMode::Optional);
  Sequence contextSeq;
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(
          Invoke([]() { return IOBuf::copyBuffer("certcontext"); }));

  EXPECT_CALL(
      *clientLeafCert_,
      verify(
          SignatureScheme::ecdsa_secp256r1_sha256,
          CertificateVerifyContext::Client,
          RangeMatches("certcontext"),
          RangeMatches("signature")))
      .InSequence(contextSeq)
      .WillOnce(Throw(
          FizzException("verify failed", AlertDescription::bad_record_mac)));

  auto actions = getActions(
      detail::processEvent(state_, TestMessages::certificateVerify()));

  expectError<FizzException>(
      actions, AlertDescription::bad_record_mac, "verify failed");
}

TEST_F(ServerProtocolTest, TestOptionalCertificateVerifyVerifierFailure) {
  setUpExpectingCertificateVerify();
  context_->setClientAuthMode(ClientAuthMode::Optional);
  Sequence contextSeq;
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(
          Invoke([]() { return IOBuf::copyBuffer("certcontext"); }));

  EXPECT_CALL(
      *clientLeafCert_,
      verify(
          SignatureScheme::ecdsa_secp256r1_sha256,
          CertificateVerifyContext::Client,
          RangeMatches("certcontext"),
          RangeMatches("signature")))
      .InSequence(contextSeq);

  EXPECT_CALL(*certVerifier_, verify(_))
      .InSequence(contextSeq)
      .WillOnce(Throw(
          FizzException("verifier failed", AlertDescription::bad_certificate)));

  auto actions = getActions(
      detail::processEvent(state_, TestMessages::certificateVerify()));

  expectError<FizzException>(
      actions, AlertDescription::bad_certificate, "verifier failed");
}

TEST_F(ServerProtocolTest, TestCertificateVerifyVerifierGenericFailure) {
  setUpExpectingCertificateVerify();
  Sequence contextSeq;
  EXPECT_CALL(*mockHandshakeContext_, getHandshakeContext())
      .InSequence(contextSeq)
      .WillRepeatedly(
          Invoke([]() { return IOBuf::copyBuffer("certcontext"); }));

  EXPECT_CALL(
      *clientLeafCert_,
      verify(
          SignatureScheme::ecdsa_secp256r1_sha256,
          CertificateVerifyContext::Client,
          RangeMatches("certcontext"),
          RangeMatches("signature")))
      .InSequence(contextSeq);

  EXPECT_CALL(*certVerifier_, verify(_))
      .InSequence(contextSeq)
      .WillOnce(Throw(std::runtime_error("oops")));

  auto actions = getActions(
      detail::processEvent(state_, TestMessages::certificateVerify()));

  expectError<FizzException>(
      actions,
      AlertDescription::bad_certificate,
      "client certificate failure: oops");
}

TEST_F(ServerProtocolTest, TestEarlyWriteError) {
  setUpAcceptingData();
  auto actions = getActions(
      ServerStateMachine().processEarlyAppWrite(state_, EarlyAppWrite()));
  expectError<FizzException>(
      actions, AlertDescription::unexpected_message, "invalid event");
}

TEST_F(ServerProtocolTest, TestDecodeErrorAlert) {
  setUpAcceptingData();
  EXPECT_CALL(*appRead_, read(_))
      .WillOnce(Invoke([](auto &&) -> folly::Optional<TLSMessage> {
        throw std::runtime_error("read record layer error");
      }));
  folly::IOBufQueue buf;
  auto actions =
      getActions(ServerStateMachine().processSocketData(state_, buf));
  auto exc = expectError<FizzException>(
      actions, AlertDescription::decode_error, "read record layer error");

  ASSERT_TRUE(exc.getAlert().hasValue());
  EXPECT_EQ(AlertDescription::decode_error, exc.getAlert().value());
}

TEST_F(ServerProtocolTest, TestSocketDataFizzExceptionAlert) {
  setUpAcceptingData();
  EXPECT_CALL(*appRead_, read(_))
      .WillOnce(Invoke([](auto &&) -> folly::Optional<TLSMessage> {
        throw FizzException(
            "arbitrary fizzexception with alert",
            AlertDescription::internal_error);
      }));
  folly::IOBufQueue buf;
  auto actions =
      getActions(ServerStateMachine().processSocketData(state_, buf));
  auto exc = expectError<FizzException>(
      actions,
      AlertDescription::internal_error,
      "arbitrary fizzexception with alert");

  ASSERT_TRUE(exc.getAlert().hasValue());
  EXPECT_EQ(AlertDescription::internal_error, exc.getAlert().value());
}

TEST_F(ServerProtocolTest, TestSocketDataFizzExceptionNoAlert) {
  setUpAcceptingData();
  EXPECT_CALL(*appRead_, read(_))
      .WillOnce(Invoke([](auto &&) -> folly::Optional<TLSMessage> {
        throw FizzException(
            "arbitrary fizzexception without alert", folly::none);
      }));
  folly::IOBufQueue buf;
  auto actions =
      getActions(ServerStateMachine().processSocketData(state_, buf));
  auto exc = expectError<FizzException>(
      actions, folly::none, "arbitrary fizzexception without alert");

  EXPECT_FALSE(exc.getAlert().hasValue());
}
} // namespace test
} // namespace server
} // namespace fizz
