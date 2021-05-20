#include "archive.hpp"
#include "dfa.hpp"
#include "error.hpp"

#include <cassert>
#include <execution>
#include <fstream>
#include <iostream>
#include <queue>
#include <set>
#include <sstream>
#include <thread>

#include <spdlog/spdlog.h>
#include <CLI/CLI.hpp>
#include <tfhe++.hpp>

void do_genkey(const std::string &output_filename)
{
    SecretKey skey;
    writeToArchive(output_filename, skey);
}

void do_genbkey(const std::string &skey_filename,
                const std::string &output_filename)
{
    auto skey = readFromArchive<SecretKey>(skey_filename);
    auto bkey = std::make_shared<GateKey>(skey);
    writeToArchive(output_filename, bkey);
}

void do_enc(const std::string &skey_filename, const std::string &input_filename,
            const std::string &output_filename)
{
    auto skey = readFromArchive<SecretKey>(skey_filename);

    std::ifstream ifs{input_filename};
    assert(ifs);
    std::vector<TRGSWLvl1FFT> data;
    while (ifs) {
        int ch = ifs.get();
        if (ch == EOF)
            break;
        for (int i = 0; i < 8; i++) {
            bool b = ((static_cast<uint8_t>(ch) >> i) & 1u) != 0;
            data.push_back(encrypt_bit_to_TRGSWLvl1FFT(b, skey));
        }
    }

    writeToArchive(output_filename, data);
}

void do_run_offline_dfa(
    const std::string &spec_filename, const std::string &input_filename,
    const std::string &output_filename,
    const std::optional<std::string> &bkey_filename = std::nullopt)
{
    ReversedTRGSWLvl1InputStreamFromCtxtFile input_stream{input_filename};

    Graph gr{spec_filename};
    gr.reserve_states_at_depth(input_stream.size());

    auto bkey = (bkey_filename
                     ? readFromArchive<std::shared_ptr<GateKey>>(*bkey_filename)
                     : nullptr);

    OfflineFARunner runner{gr, input_stream, bkey};
    runner.eval();

    writeToArchive(output_filename, runner.result());
}

void do_dec(const std::string &skey_filename, const std::string &input_filename)
{
    auto skey = readFromArchive<SecretKey>(skey_filename);
    auto enc_res = readFromArchive<TLWELvl1>(input_filename);
    bool res = TFHEpp::tlweSymDecrypt<Lvl1>(enc_res, skey.key.lvl1);
    spdlog::info("Result (bool): {}", res);
}

int main(int argc, char **argv)
{
    CLI::App app{"Homomorphic Final Answer"};
    app.require_subcommand();

    enum class TYPE {
        GENKEY,
        GENBKEY,
        ENC,
        RUN_OFFLINE_DFA,
        DEC,
    } type;

    std::optional<std::string> spec, skey, bkey, input, output;

    {
        CLI::App *genkey = app.add_subcommand("genkey", "Generate secret key");
        genkey->parse_complete_callback([&] { type = TYPE::GENKEY; });
        genkey->add_option("--out", output)->required();
    }
    {
        CLI::App *genbkey = app.add_subcommand(
            "genbkey", "Generate bootstrapping key from secret key");
        genbkey->parse_complete_callback([&] { type = TYPE::GENBKEY; });
        genbkey->add_option("--key", skey)
            ->required()
            ->check(CLI::ExistingFile);
        genbkey->add_option("--out", output)->required();
    }
    {
        CLI::App *enc = app.add_subcommand("enc", "Encrypt input file");
        enc->parse_complete_callback([&] { type = TYPE::ENC; });
        enc->add_option("--key", skey)->required()->check(CLI::ExistingFile);
        enc->add_option("--in", input)->required()->check(CLI::ExistingFile);
        enc->add_option("--out", output)->required();
    }
    {
        CLI::App *run =
            app.add_subcommand("run-offline-dfa", "Run offline DFA");
        run->parse_complete_callback([&] { type = TYPE::RUN_OFFLINE_DFA; });
        run->add_option("--bkey", bkey)->check(CLI::ExistingFile);
        run->add_option("--spec", spec)->required()->check(CLI::ExistingFile);
        run->add_option("--in", input)->required()->check(CLI::ExistingFile);
        run->add_option("--out", output)->required();
    }
    {
        CLI::App *dec = app.add_subcommand("dec", "Decrypt input file");
        dec->parse_complete_callback([&] { type = TYPE::DEC; });
        dec->add_option("--key", skey)->required()->check(CLI::ExistingFile);
        dec->add_option("--in", input)->required()->check(CLI::ExistingFile);
    }

    CLI11_PARSE(app, argc, argv);

    switch (type) {
    case TYPE::GENKEY:
        assert(output);
        do_genkey(*output);
        break;

    case TYPE::GENBKEY:
        assert(skey && output);
        do_genbkey(*skey, *output);
        break;

    case TYPE::ENC:
        assert(skey && input && output);
        do_enc(*skey, *input, *output);
        break;

    case TYPE::RUN_OFFLINE_DFA:
        assert(spec && input && output);
        do_run_offline_dfa(*spec, *input, *output, bkey);
        break;

    case TYPE::DEC:
        assert(skey && input);
        do_dec(*skey, *input);
        break;
    }

    return 0;
}
