#include <iostream>                    // std::cout, std::flush
#include <fstream>                     // std::ifstream, std::ofstream
#include <filesystem>                  // std::filesystem
#include <string>                      // std::string
#include <sstream>                     // std::stringstream
#include <tuple>                       // std::tuple
#include <vector>                      // std::vector
#include <utility>                     // std::pair
#include <cmath>                       // std::ceil
// For External Library
#include <torch/torch.h>               // torch
#include <boost/program_options.hpp>   // boost::program_options
// For Original Header
#include "loss.hpp"                    // Loss
#include "networks.hpp"                // Encoder, Decoder, GAN_Discriminator
#include "transforms.hpp"              // transforms::Compose
#include "datasets.hpp"                // datasets::ImageFolderWithPaths
#include "dataloader.hpp"              // DataLoader::ImageFolderWithPaths
#include "visualizer.hpp"              // visualizer
#include "progress.hpp"                // progress

// Define Namespace
namespace fs = std::filesystem;
namespace po = boost::program_options;

// Function Prototype
void valid(po::variables_map &vm, DataLoader::ImageFolderWithPaths &valid_dataloader, torch::Device &device, Loss &criterion_adv, Loss &criterion_con, Loss &criterion_enc, torch::nn::BCEWithLogitsLoss &criterion_dis, Encoder &enc1, Encoder &enc2, Decoder &dec, GAN_Discriminator &dis, const size_t epoch, visualizer::graph &writer_rec, visualizer::graph &writer_gan, visualizer::graph &writer_dis);


// -------------------
// Training Function
// -------------------
void train(po::variables_map &vm, torch::Device &device, Encoder &enc1, Encoder &enc2, Decoder &dec, GAN_Discriminator &dis, std::vector<transforms::Compose*> &transform){

    constexpr bool train_shuffle = true;  // whether to shuffle the training dataset
    constexpr size_t train_workers = 4;  // the number of workers to retrieve data from the training dataset
    constexpr bool valid_shuffle = true;  // whether to shuffle the validation dataset
    constexpr size_t valid_workers = 4;  // the number of workers to retrieve data from the validation dataset
    constexpr size_t save_sample_iter = 50;  // the frequency of iteration to save sample images
    constexpr std::string_view extension = "jpg";  // the extension of file name to save sample images
    constexpr std::pair<float, float> output_range = {-1.0, 1.0};  // range of the value in output images

    // -----------------------------------
    // a0. Initialization and Declaration
    // -----------------------------------

    size_t epoch, iter;
    size_t total_iter;
    size_t start_epoch, total_epoch;
    size_t mini_batch_size;
    std::string date, date_out;
    std::string buff, latest;
    std::string checkpoint_dir, save_images_dir, path;
    std::string dataroot, valid_dataroot;
    std::stringstream ss;
    std::ifstream infoi;
    std::ofstream ofs, init, infoo;
    std::tuple<torch::Tensor, std::vector<std::string>> mini_batch;
    torch::Tensor image, fake_image, z, z_rec, pair;
    torch::Tensor dis_real_feat, dis_fake_feat;
    torch::Tensor dis_real_out, dis_fake_out;
    torch::Tensor loss, adv_loss, con_loss, enc_loss, dis_loss, dis_real_loss, dis_fake_loss;
    torch::Tensor label_real, label_fake;
    datasets::ImageFolderWithPaths dataset, valid_dataset;
    DataLoader::ImageFolderWithPaths dataloader, valid_dataloader;
    visualizer::graph train_loss_rec, train_loss_gan, train_loss_dis;
    visualizer::graph valid_loss_rec, valid_loss_gan, valid_loss_dis;
    progress::display *show_progress;
    progress::irregular irreg_progress;


    // -----------------------------------
    // a1. Preparation
    // -----------------------------------

    // (1) Get Training Dataset
    dataroot = "datasets/" + vm["dataset"].as<std::string>() + "/" + vm["train_dir"].as<std::string>();
    dataset = datasets::ImageFolderWithPaths(dataroot, transform);
    dataloader = DataLoader::ImageFolderWithPaths(dataset, vm["batch_size"].as<size_t>(), /*shuffle_=*/train_shuffle, /*num_workers_=*/train_workers);
    std::cout << "total training images : " << dataset.size() << std::endl;

    // (2) Get Validation Dataset
    if (vm["valid"].as<bool>()){
        valid_dataroot = "datasets/" + vm["dataset"].as<std::string>() + "/" + vm["valid_dir"].as<std::string>();
        valid_dataset = datasets::ImageFolderWithPaths(valid_dataroot, transform);
        valid_dataloader = DataLoader::ImageFolderWithPaths(valid_dataset, vm["valid_batch_size"].as<size_t>(), /*shuffle_=*/valid_shuffle, /*num_workers_=*/valid_workers);
        std::cout << "total validation images : " << valid_dataset.size() << std::endl;
    }

    // (3) Set Optimizer Method
    auto enc1_optimizer = torch::optim::Adam(enc1->parameters(), torch::optim::AdamOptions(vm["lr_enc"].as<float>()).betas({vm["beta1"].as<float>(), vm["beta2"].as<float>()}));
    auto enc2_optimizer = torch::optim::Adam(enc2->parameters(), torch::optim::AdamOptions(vm["lr_enc"].as<float>()).betas({vm["beta1"].as<float>(), vm["beta2"].as<float>()}));
    auto dec_optimizer = torch::optim::Adam(dec->parameters(), torch::optim::AdamOptions(vm["lr_dec"].as<float>()).betas({vm["beta1"].as<float>(), vm["beta2"].as<float>()}));
    auto dis_optimizer = torch::optim::Adam(dis->parameters(), torch::optim::AdamOptions(vm["lr_dis"].as<float>()).betas({vm["beta1"].as<float>(), vm["beta2"].as<float>()}));

    // (4) Set Loss Function
    auto criterion_adv = Loss(vm["loss_adv"].as<std::string>());
    auto criterion_con = Loss(vm["loss_con"].as<std::string>());
    auto criterion_enc = Loss(vm["loss_enc"].as<std::string>());
    auto criterion_dis = torch::nn::BCEWithLogitsLoss(torch::nn::BCEWithLogitsLossOptions().reduction(torch::kMean));

    // (5) Make Directories
    checkpoint_dir = "checkpoints/" + vm["dataset"].as<std::string>();
    path = checkpoint_dir + "/models";  fs::create_directories(path);
    path = checkpoint_dir + "/optims";  fs::create_directories(path);
    path = checkpoint_dir + "/log";  fs::create_directories(path);
    save_images_dir = checkpoint_dir + "/samples";  fs::create_directories(save_images_dir);

    // (6) Set Training Loss for Graph
    path = checkpoint_dir + "/graph";
    train_loss_rec = visualizer::graph(path, /*gname_=*/"train_loss_rec", /*label_=*/{"Contextual", "Encoder"});
    train_loss_gan = visualizer::graph(path, /*gname_=*/"train_loss_gan", /*label_=*/{"Adversarial", "Discriminator"});
    train_loss_dis = visualizer::graph(path, /*gname_=*/"train_loss_dis", /*label_=*/{"Total", "Real", "Fake"});
    if (vm["valid"].as<bool>()){
        valid_loss_rec = visualizer::graph(path, /*gname_=*/"valid_loss_rec", /*label_=*/{"Contextual", "Encoder"});
        valid_loss_gan = visualizer::graph(path, /*gname_=*/"valid_loss_gan", /*label_=*/{"Adversarial", "Discriminator"});
        valid_loss_dis = visualizer::graph(path, /*gname_=*/"valid_loss_dis", /*label_=*/{"Total", "Real", "Fake"});
    }
    
    // (7) Get Weights and File Processing
    if (vm["train_load_epoch"].as<std::string>() == ""){
        enc1->apply(weights_init);
        enc2->apply(weights_init);
        dec->apply(weights_init);
        dis->apply(weights_init);
        ofs.open(checkpoint_dir + "/log/train.txt", std::ios::out);
        if (vm["valid"].as<bool>()){
            init.open(checkpoint_dir + "/log/valid.txt", std::ios::trunc);
            init.close();
        }
        start_epoch = 0;
    }
    else{
        path = checkpoint_dir + "/models/epoch_" + vm["train_load_epoch"].as<std::string>() + "_enc1.pth";  torch::load(enc1, path);
        path = checkpoint_dir + "/models/epoch_" + vm["train_load_epoch"].as<std::string>() + "_enc2.pth";  torch::load(enc2, path);
        path = checkpoint_dir + "/models/epoch_" + vm["train_load_epoch"].as<std::string>() + "_dec.pth";  torch::load(dec, path);
        path = checkpoint_dir + "/models/epoch_" + vm["train_load_epoch"].as<std::string>() + "_dis.pth";  torch::load(dis, path);
        path = checkpoint_dir + "/optims/epoch_" + vm["train_load_epoch"].as<std::string>() + "_enc1.pth";  torch::load(enc1_optimizer, path);
        path = checkpoint_dir + "/optims/epoch_" + vm["train_load_epoch"].as<std::string>() + "_enc2.pth";  torch::load(enc2_optimizer, path);
        path = checkpoint_dir + "/optims/epoch_" + vm["train_load_epoch"].as<std::string>() + "_dec.pth";  torch::load(dec_optimizer, path);
        path = checkpoint_dir + "/optims/epoch_" + vm["train_load_epoch"].as<std::string>() + "_dis.pth";  torch::load(dis_optimizer, path);
        ofs.open(checkpoint_dir + "/log/train.txt", std::ios::app);
        ofs << std::endl << std::endl;
        if (vm["train_load_epoch"].as<std::string>() == "latest"){
            infoi.open(checkpoint_dir + "/models/info.txt", std::ios::in);
            std::getline(infoi, buff);
            infoi.close();
            latest = "";
            for (auto &c : buff){
                if (('0' <= c) && (c <= '9')){
                    latest += c;
                }
            }
            start_epoch = std::stoi(latest);
        }
        else{
            start_epoch = std::stoi(vm["train_load_epoch"].as<std::string>());
        }
    }

    // (8) Display Date
    date = progress::current_date();
    date = progress::separator_center("Train Loss (" + date + ")");
    std::cout << std::endl << std::endl << date << std::endl;
    ofs << date << std::endl;


    // -----------------------------------
    // a2. Training Model
    // -----------------------------------
    
    // (1) Set Parameters
    start_epoch++;
    total_iter = std::ceil((float)dataset.size() / (float)vm["batch_size"].as<size_t>());
    total_epoch = vm["epochs"].as<size_t>();

    // (2) Training per Epoch
    mini_batch_size = 0;
    irreg_progress.restart(start_epoch - 1, total_epoch);
    for (epoch = start_epoch; epoch <= total_epoch; epoch++){

        enc1->train();
        enc2->train();
        dec->train();
        dis->train();
        ofs << std::endl << "epoch:" << epoch << '/' << total_epoch << std::endl;
        show_progress = new progress::display(/*count_max_=*/total_iter, /*epoch=*/{epoch, total_epoch}, /*loss_=*/{"adv", "con", "enc", "D_Real", "D_Fake"});

        // -----------------------------------
        // b1. Mini Batch Learning
        // -----------------------------------
        while (dataloader(mini_batch)){

            image = std::get<0>(mini_batch).to(device);
            mini_batch_size = image.size(0);

            // --------------------------------------------------------
            // c1. Discriminator, Generator and Encoder Training Phase
            // --------------------------------------------------------

            // (1) Set Target Label
            label_real = torch::full({(long int)mini_batch_size}, /*value=*/1.0, torch::TensorOptions().dtype(torch::kFloat)).to(device);
            label_fake = torch::full({(long int)mini_batch_size}, /*value=*/0.0, torch::TensorOptions().dtype(torch::kFloat)).to(device);

            // (2) Discriminator Training
            z = enc1->forward(image);
            fake_image = dec->forward(z);
            dis_fake_out = dis->forward(fake_image.detach()).first.view({-1});
            dis_real_out = dis->forward(image).first.view({-1});
            dis_fake_loss = criterion_dis(dis_fake_out, label_fake);
            dis_real_loss = criterion_dis(dis_real_out, label_real);
            dis_loss = dis_real_loss + dis_fake_loss;
            dis_optimizer.zero_grad();
            dis_loss.backward();
            dis_optimizer.step();

            // (3) Generator and Encoder Training
            dis_fake_feat = dis->forward(fake_image).second;
            dis_real_feat = dis->forward(image).second;
            z_rec = enc2->forward(fake_image);
            adv_loss = criterion_adv(dis_fake_feat, dis_real_feat) * vm["Lambda_adv"].as<float>();
            con_loss = criterion_con(fake_image, image) * vm["Lambda_con"].as<float>();
            enc_loss = criterion_enc(z_rec, z) * vm["Lambda_enc"].as<float>();
            loss = adv_loss + con_loss + enc_loss;
            enc1_optimizer.zero_grad();
            enc2_optimizer.zero_grad();
            dec_optimizer.zero_grad();
            loss.backward();
            enc1_optimizer.step();
            enc2_optimizer.step();
            dec_optimizer.step();

            // -----------------------------------
            // c2. Record Loss (iteration)
            // -----------------------------------
            show_progress->increment(/*loss_value=*/{adv_loss.item<float>(), con_loss.item<float>(), enc_loss.item<float>(), dis_real_loss.item<float>(), dis_fake_loss.item<float>()});
            ofs << "iters:" << show_progress->get_iters() << '/' << total_iter << ' ' << std::flush;
            ofs << "adv:" << adv_loss.item<float>() << "(ave:" <<  show_progress->get_ave(0) << ") " << std::flush;
            ofs << "con:" << con_loss.item<float>() << "(ave:" <<  show_progress->get_ave(1) << ") " << std::flush;
            ofs << "enc:" << enc_loss.item<float>() << "(ave:" <<  show_progress->get_ave(2) << ") " << std::flush;
            ofs << "D_Real:" << dis_real_loss.item<float>() << "(ave:" <<  show_progress->get_ave(3) << ") " << std::flush;
            ofs << "D_Fake:" << dis_fake_loss.item<float>() << "(ave:" <<  show_progress->get_ave(4) << ")" << std::endl;

            // -----------------------------------
            // c3. Save Sample Images
            // -----------------------------------
            iter = show_progress->get_iters();
            if (iter % save_sample_iter == 1){
                ss.str(""); ss.clear(std::stringstream::goodbit);
                ss << save_images_dir << "/epoch_" << epoch << "-iter_" << iter << '.' << extension;
                pair = torch::cat({image, fake_image}, /*dim=*/0);
                visualizer::save_image(pair.detach(), ss.str(), /*range=*/output_range, /*cols=*/mini_batch_size);
            }

        }

        // -----------------------------------
        // b2. Record Loss (epoch)
        // -----------------------------------
        train_loss_rec.plot(/*base=*/epoch, /*value=*/{show_progress->get_ave(1), show_progress->get_ave(2)});
        train_loss_gan.plot(/*base=*/epoch, /*value=*/{show_progress->get_ave(0), show_progress->get_ave(3) + show_progress->get_ave(4)});
        train_loss_dis.plot(/*base=*/epoch, /*value=*/{show_progress->get_ave(3) + show_progress->get_ave(4), show_progress->get_ave(3), show_progress->get_ave(4)});

        // -----------------------------------
        // b3. Save Sample Images
        // -----------------------------------
        ss.str(""); ss.clear(std::stringstream::goodbit);
        ss << save_images_dir << "/epoch_" << epoch << "-iter_" << show_progress->get_iters() << '.' << extension;
        pair = torch::cat({image, fake_image}, /*dim=*/0);
        visualizer::save_image(pair.detach(), ss.str(), /*range=*/output_range, /*cols=*/mini_batch_size);
        delete show_progress;
        
        // -----------------------------------
        // b4. Validation Mode
        // -----------------------------------
        if (vm["valid"].as<bool>() && ((epoch - 1) % vm["valid_freq"].as<size_t>() == 0)){
            valid(vm, valid_dataloader, device, criterion_adv, criterion_con, criterion_enc, criterion_dis, enc1, enc2, dec, dis, epoch, valid_loss_rec, valid_loss_gan, valid_loss_dis);
        }

        // -----------------------------------
        // b5. Save Model Weights and Optimizer Parameters
        // -----------------------------------
        if (epoch % vm["save_epoch"].as<size_t>() == 0){
            path = checkpoint_dir + "/models/epoch_" + std::to_string(epoch) + "_enc1.pth";  torch::save(enc1, path);
            path = checkpoint_dir + "/models/epoch_" + std::to_string(epoch) + "_enc2.pth";  torch::save(enc2, path);
            path = checkpoint_dir + "/models/epoch_" + std::to_string(epoch) + "_dec.pth";  torch::save(dec, path);
            path = checkpoint_dir + "/models/epoch_" + std::to_string(epoch) + "_dis.pth";  torch::save(dis, path);
            path = checkpoint_dir + "/optims/epoch_" + std::to_string(epoch) + "_enc1.pth";  torch::save(enc1_optimizer, path);
            path = checkpoint_dir + "/optims/epoch_" + std::to_string(epoch) + "_enc2.pth";  torch::save(enc2_optimizer, path);
            path = checkpoint_dir + "/optims/epoch_" + std::to_string(epoch) + "_dec.pth";  torch::save(dec_optimizer, path);
            path = checkpoint_dir + "/optims/epoch_" + std::to_string(epoch) + "_dis.pth";  torch::save(dis_optimizer, path);
        }
        path = checkpoint_dir + "/models/epoch_latest_enc1.pth";  torch::save(enc1, path);
        path = checkpoint_dir + "/models/epoch_latest_enc2.pth";  torch::save(enc2, path);
        path = checkpoint_dir + "/models/epoch_latest_dec.pth";  torch::save(dec, path);
        path = checkpoint_dir + "/models/epoch_latest_dis.pth";  torch::save(dis, path);
        path = checkpoint_dir + "/optims/epoch_latest_enc1.pth";  torch::save(enc1_optimizer, path);
        path = checkpoint_dir + "/optims/epoch_latest_enc2.pth";  torch::save(enc2_optimizer, path);
        path = checkpoint_dir + "/optims/epoch_latest_dec.pth";  torch::save(dec_optimizer, path);
        path = checkpoint_dir + "/optims/epoch_latest_dis.pth";  torch::save(dis_optimizer, path);
        infoo.open(checkpoint_dir + "/models/info.txt", std::ios::out);
        infoo << "latest = " << epoch << std::endl;
        infoo.close();

        // -----------------------------------
        // b6. Show Elapsed Time
        // -----------------------------------
        if (epoch % 10 == 0){

            // -----------------------------------
            // c1. Get Output String
            // -----------------------------------
            ss.str(""); ss.clear(std::stringstream::goodbit);
            irreg_progress.nab(epoch);
            ss << "elapsed = " << irreg_progress.get_elap() << '(' << irreg_progress.get_sec_per() << "sec/epoch)   ";
            ss << "remaining = " << irreg_progress.get_rem() << "   ";
            ss << "now = " << irreg_progress.get_date() << "   ";
            ss << "finish = " << irreg_progress.get_date_fin();
            date_out = ss.str();

            // -----------------------------------
            // c2. Terminal Output
            // -----------------------------------
            std::cout << date_out << std::endl << progress::separator() << std::endl;
            ofs << date_out << std::endl << progress::separator() << std::endl;

        }

    }

    // Post Processing
    ofs.close();

    // End Processing
    return;

}
