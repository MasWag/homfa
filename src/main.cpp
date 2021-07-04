#include "archive.hpp"
#include "error.hpp"
#include "offline_dfa.hpp"
#include "online_dfa.hpp"

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

// Bootstrapping key in the broad sense
struct BKey {
    std::shared_ptr<GateKey> gkey;
    std::shared_ptr<TFHEpp::TLWE2TRLWEIKSKey<TFHEpp::lvl11param>>
        tlwel1_trlwel1_ikskey;

    BKey()
    {
    }

    BKey(const SecretKey &skey)
        : gkey(std::make_shared<GateKey>(skey)),
          tlwel1_trlwel1_ikskey(
              std::make_shared<TFHEpp::TLWE2TRLWEIKSKey<TFHEpp::lvl11param>>())
    {
        TFHEpp::tlwe2trlweikskkgen<TFHEpp::lvl11param>(*tlwel1_trlwel1_ikskey,
                                                       skey);
    }

    template <class Archive>
    void serialize(Archive &ar)
    {
        ar(gkey, tlwel1_trlwel1_ikskey);
    }
};

void do_genkey(const std::string &output_filename)
{
    SecretKey skey;
    write_to_archive(output_filename, skey);
}

void do_genbkey(const std::string &skey_filename,
                const std::string &output_filename)
{
    auto skey = read_from_archive<SecretKey>(skey_filename);
    BKey bkey{skey};
    write_to_archive(output_filename, bkey);
}

void do_enc(const std::string &skey_filename, const std::string &input_filename,
            const std::string &output_filename)
{
    auto skey = read_from_archive<SecretKey>(skey_filename);

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

    write_to_archive(output_filename, data);
}

void do_run_offline_dfa(
    const std::string &spec_filename, const std::string &input_filename,
    const std::string &output_filename,
    const std::optional<std::string> &bkey_filename = std::nullopt)
{
    ReversedTRGSWLvl1InputStreamFromCtxtFile input_stream{input_filename};

    Graph gr = Graph::from_file(spec_filename).minimized();
    gr.reserve_states_at_depth(input_stream.size());

    auto bkey = read_from_archive<BKey>(*bkey_filename);

    spdlog::info("Parameter:");
    spdlog::info("\tMode:\t{}", "Offline FA Runner");
    spdlog::info("\tInput size:\t{}", input_stream.size());
    spdlog::info("\tState size:\t{}", gr.size());
    spdlog::info("\tConcurrency:\t{}", std::thread::hardware_concurrency());
    {
        size_t total_cnt_cmux = 0;
        for (size_t j = 0; j < input_stream.size(); j++)
            total_cnt_cmux += gr.states_at_depth(j).size();
        spdlog::info("\tTotal #CMUX:\t{}", total_cnt_cmux);
    }
    spdlog::info("");

    OfflineDFARunner runner{gr, input_stream, bkey.gkey};
    runner.eval();

    write_to_archive(output_filename, runner.result());
}

void do_run_online_dfa(
    const std::string &spec_filename, const std::string &input_filename,
    const std::string &output_filename,
    const std::optional<std::string> &bkey_filename = std::nullopt)
{
    TRGSWLvl1InputStreamFromCtxtFile input_stream{input_filename};
    Graph gr = Graph::from_file(spec_filename);
    auto bkey = read_from_archive<BKey>(*bkey_filename);
    OnlineDFARunner runner{gr, bkey.gkey};

    spdlog::info("Parameter:");
    spdlog::info("\tMode:\t{}", "Online FA Runner1 (qtrlwe)");
    spdlog::info("\tState size:\t{}", gr.size());
    spdlog::info("\tConcurrency:\t{}", std::thread::hardware_concurrency());
    // spdlog::info("\tBootstrap interval:\t{}", bootstrap_interval_);
    spdlog::info("");

    for (size_t i = 0; input_stream.size() != 0; i++) {
        spdlog::debug("Processing input {}", i);
        runner.eval_one(input_stream.next());
    }

    write_to_archive(output_filename, runner.result());
}

void do_run_online_dfa2(
    const std::string &spec_filename, const std::string &input_filename,
    const std::string &output_filename,
    const std::optional<std::string> &bkey_filename = std::nullopt)
{
    TRGSWLvl1InputStreamFromCtxtFile input_stream{input_filename};
    Graph gr = Graph::from_file(spec_filename).reversed();
    auto bkey = read_from_archive<BKey>(*bkey_filename);
    OnlineDFARunner2 runner{gr, bkey.gkey};

    spdlog::info("Parameter:");
    spdlog::info("\tMode:\t{}", "Online FA Runner2 (reversed)");
    spdlog::info("\tInput size:\t{}", input_stream.size());
    spdlog::info("\tState size:\t{}", gr.size());
    spdlog::info("\tConcurrency:\t{}", std::thread::hardware_concurrency());
    spdlog::info("");

    for (size_t i = 0; input_stream.size() != 0; i++) {
        spdlog::debug("Processing input {}", i);
        runner.eval_one(input_stream.next());
    }

    write_to_archive(output_filename, runner.result());
}

void do_run_online_dfa3(const std::string &spec_filename,
                        const std::string &input_filename,
                        const std::string &output_filename,
                        size_t first_lut_max_depth,
                        const std::string &bkey_filename,
                        const std::optional<std::string> &debug_skey_filename)
{
    TRGSWLvl1InputStreamFromCtxtFile input_stream{input_filename};
    Graph gr = Graph::from_file(spec_filename);

    auto bkey = read_from_archive<BKey>(bkey_filename);
    assert(bkey.gkey && bkey.tlwel1_trlwel1_ikskey);

    std::optional<SecretKey> debug_skey;
    if (debug_skey_filename)
        debug_skey.emplace(read_from_archive<SecretKey>(*debug_skey_filename));

    OnlineDFARunner3 runner{gr, first_lut_max_depth, *bkey.gkey,
                            *bkey.tlwel1_trlwel1_ikskey, debug_skey};

    spdlog::info("Parameter:");
    spdlog::info("\tMode:\t{}", "Online FA Runner3 (qtrlwe2)");
    spdlog::info("\tInput size:\t{}", input_stream.size());
    spdlog::info("\tState size:\t{}", gr.size());
    spdlog::info("\tConcurrency:\t{}", std::thread::hardware_concurrency());
    spdlog::info("\tQueue size:\t{} = {} + {}", runner.queue_size(),
                 runner.first_lut_max_depth(), runner.second_lut_max_depth());
    spdlog::info("");

    for (size_t i = 0; input_stream.size() != 0; i++) {
        spdlog::debug("Processing input {}", i);
        runner.eval_one(input_stream.next());
    }
    TLWELvl1 res = runner.result();

    write_to_archive(output_filename, res);
}

void do_dec(const std::string &skey_filename, const std::string &input_filename)
{
    auto skey = read_from_archive<SecretKey>(skey_filename);
    auto enc_res = read_from_archive<TLWELvl1>(input_filename);
    bool res = TFHEpp::tlweSymDecrypt<Lvl1>(enc_res, skey.key.lvl1);
    spdlog::info("Result (bool): {}", res);
}

void do_ltl2spec(const std::string &fml, size_t num_vars)
{
    Graph gr = Graph::from_ltl_formula(fml, num_vars).minimized();
    gr.dump(std::cout);
}

void do_ltl2dot(const std::string &fml, size_t num_vars, bool minimized,
                bool reversed, bool negated)
{
    Graph gr = Graph::from_ltl_formula(fml, num_vars);
    if (negated)
        gr = gr.negated();
    if (reversed)
        gr = gr.reversed();
    if (minimized)
        gr.minimized().dump_dot(std::cout);
    else
        gr.dump_dot(std::cout);
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
        RUN_ONLINE_DFA,
        DEC,
        LTL2SPEC,
        LTL2DOT,
    } type;

    bool verbose = false, quiet = false, minimized = false, reversed = false,
         negated;
    std::optional<std::string> spec, skey, bkey, input, output, debug_skey;
    std::string formula, online_method = "qtrlwe2";
    std::optional<size_t> num_vars;
    size_t first_lut_max_depth = 8;

    app.add_flag("--verbose", verbose, "");
    app.add_flag("--quiet", quiet, "");
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
        CLI::App *run = app.add_subcommand("run-online-dfa", "Run online DFA");
        run->parse_complete_callback([&] { type = TYPE::RUN_ONLINE_DFA; });
        run->add_option("--bkey", bkey)->check(CLI::ExistingFile);
        run->add_option("--spec", spec)->required()->check(CLI::ExistingFile);
        run->add_option("--in", input)->required()->check(CLI::ExistingFile);
        run->add_option("--out", output)->required();
        run->add_option("--method", online_method)
            ->check(CLI::IsMember({"qtrlwe", "reversed", "qtrlwe2"}));
        run->add_option("--first-lut-max-depth", first_lut_max_depth)
            ->check(CLI::PositiveNumber);
        run->add_option("--debug-secret-key", debug_skey)
            ->check(CLI::ExistingFile);
    }
    {
        CLI::App *dec = app.add_subcommand("dec", "Decrypt input file");
        dec->parse_complete_callback([&] { type = TYPE::DEC; });
        dec->add_option("--key", skey)->required()->check(CLI::ExistingFile);
        dec->add_option("--in", input)->required()->check(CLI::ExistingFile);
    }
    {
        CLI::App *ltl2spec = app.add_subcommand(
            "ltl2spec", "Convert LTL to spec format for HomFA");
        ltl2spec->parse_complete_callback([&] { type = TYPE::LTL2SPEC; });
        ltl2spec->add_option("formula", formula)->required();
        ltl2spec->add_option("#vars", num_vars)->required();
    }
    {
        CLI::App *ltl2dot =
            app.add_subcommand("ltl2dot", "Convert LTL to dot script");
        ltl2dot->parse_complete_callback([&] { type = TYPE::LTL2DOT; });
        ltl2dot->add_flag("--minimized", minimized);
        ltl2dot->add_flag("--reversed", reversed);
        ltl2dot->add_flag("--negated", negated);
        ltl2dot->add_option("formula", formula)->required();
        ltl2dot->add_option("#vars", num_vars)->required();
    }

    CLI11_PARSE(app, argc, argv);

    if (quiet)
        spdlog::set_level(spdlog::level::err);
    if (verbose)
        spdlog::set_level(spdlog::level::debug);

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

    case TYPE::RUN_ONLINE_DFA:
        assert(spec && input && output);
        if (online_method == "qtrlwe") {
            do_run_online_dfa(*spec, *input, *output, bkey);
        }
        else if (online_method == "reversed") {
            do_run_online_dfa2(*spec, *input, *output, bkey);
        }
        else {
            assert(online_method == "qtrlwe2");
            assert(bkey);
            do_run_online_dfa3(*spec, *input, *output, first_lut_max_depth,
                               *bkey, debug_skey);
        }
        break;

    case TYPE::DEC:
        assert(skey && input);
        do_dec(*skey, *input);
        break;

    case TYPE::LTL2SPEC:
        assert(num_vars);
        do_ltl2spec(formula, *num_vars);
        break;

    case TYPE::LTL2DOT:
        assert(num_vars);
        do_ltl2dot(formula, *num_vars, minimized, reversed, negated);
        break;
    }

    return 0;
}
