#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

template <typename T>
bool checkMin(T first, T second)
{
    return juce::jmin(first, second) == std::min(first, second);
}

template <typename T>
bool checkMax(T first, T second)
{
    return juce::jmax(first, second) == std::max(first, second);
}

TEST_CASE("Test that juce::jmin works")
{
    REQUIRE(checkMin(5, 7));
    REQUIRE(checkMin(12, 3));
    REQUIRE(checkMin(5.31, 5.42));
}

TEST_CASE("Test that juce::jmax works")
{
    REQUIRE(checkMax(5, 7));
    REQUIRE(checkMax(12, 3));
    REQUIRE(checkMax(5.31, 5.42));
}

static juce::String buildIs05PatchBody(const std::string& senderId, const std::string& destIp, uint16_t destPort)
{
    juce::DynamicObject staged;
    staged.setProperty("sender_id", senderId);
    staged.setProperty("master_enable", true);

    juce::Array<juce::var> tpArray;
    juce::DynamicObject* tp = new juce::DynamicObject();
    tp->setProperty("destination_ip", destIp);
    tp->setProperty("destination_port", static_cast<int>(destPort));
    tp->setProperty("rtp_enabled", true);
    tpArray.add(juce::var(tp));
    staged.setProperty("transport_params", tpArray);

    return juce::JSON::toString(juce::var(&staged), true);
}

TEST_CASE("IS-05 staged patch body has expected fields")
{
    auto body = buildIs05PatchBody("sender-uuid", "239.0.0.1", 5004);
    auto parsed = juce::JSON::parse(body);
    REQUIRE(parsed.isObject());
    auto* obj = parsed.getDynamicObject();
    REQUIRE(obj != nullptr);
    REQUIRE(obj->getProperty("sender_id").toString() == "sender-uuid");
    REQUIRE(obj->getProperty("master_enable") == true);

    auto tp = obj->getProperty("transport_params");
    REQUIRE(tp.isArray());
    REQUIRE(tp.getArray()->size() == 1);
    auto* tpObj = (*tp.getArray())[0].getDynamicObject();
    REQUIRE(tpObj != nullptr);
    REQUIRE(tpObj->getProperty("destination_ip").toString() == "239.0.0.1");
    REQUIRE(tpObj->getProperty("destination_port") == 5004);
    REQUIRE(tpObj->getProperty("rtp_enabled") == true);
}