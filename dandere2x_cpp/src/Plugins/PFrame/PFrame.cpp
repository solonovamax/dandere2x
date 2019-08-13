//
// Created by https://github.com/CardinalPanda
//
//Licensed under the GNU General Public License Version 3 (GNU GPL v3),
//    available at: https://www.gnu.org/licenses/gpl-3.0.txt

#include "Plugins/PFrame/PFrame.h"
#include <omp.h>

PFrame::PFrame(std::shared_ptr<Image> image1, std::shared_ptr<Image> image2, std::shared_ptr<Image> image2_compressed,
               unsigned int block_size, std::string p_frame_file, std::string difference_file,
               int step_size) {

    this->image1 = image1;
    this->image2 = image2;
    this->image2_compressed = image2_compressed;
    this->step_size = step_size;
    this->max_checks = 128; //prevent diamond search from going on forever
    this->block_size = block_size;
    this->width = image1->width;
    this->height = image1->height;
    this->dif = nullptr;
    this->p_frame_file = p_frame_file;
    this->difference_file = difference_file;
    this->matched_blocks.resize(this->width / block_size, std::vector<Block>(this->height / block_size));
    this->count = 0;

    //Sanity Checks
    if (image1->height != image2->height || image1->width != image2->width)
        throw std::invalid_argument("PDifference image resolution does not match!");
}


/**
 * Run will modify image2 when the draw over command is called.
 */
void PFrame::run() {
    double psnr = ImageUtils::psnr(*image1, *image2);

    //if the psnr is really low between two images, don't bother trying to match blocks, as they're
    //probably wont be anyways to match
    if (psnr < 10) {
        std::cout << "PSNR " << psnr << std::endl;
        std::cout << "PSNR is low - not going to match blocks" << std::endl;
        blocks.clear();
    }
        //if the PSNR is acceptable, try matching all the blocks.
    else {
        match_all_blocks();

        std::cout << "got here  " << std::endl;
        //if the amount of blocks matched is greater than 85% of the total blocks, throw away all the blocks.
        //At a certain point it's just easier / faster to redraw a scene rather than trying to piece it back together.
        int max_blocks_possible = (this->height * this->width) / (this->block_size * this->block_size);

        if ((max_blocks_possible - this->count) > (.85) * max_blocks_possible) {
            std::cout << "Too many missing blocks - conducting redraw" << std::endl;
            //this->matched_blocks.resize(this->width / block_size, std::vector<Block>(this->height / block_size));
            this->count = 0;
        }
    }

    draw_over();
    std::cout << "matched blocks: " << this->count << std::endl;

}


/**
 * Saves the blocks (if they are there) into a relevent text files
 */
void PFrame::save() {
    if (this->count != 0) { //if parts of frame2 can be made of frame1, create frame2'
        create_difference();
        this->dif->write(difference_file);
        this->write(p_frame_file);
    } else { //if parts of frame2 cannot be made from frame1, just copy frame2.
        dandere2x::write_empty(difference_file);
        dandere2x::write_empty(p_frame_file);
    }
}


void PFrame::create_difference() {
    dif = std::make_shared<Differences>(blocks, block_size, image2);
    dif->run();
}

// Make edits to image2 using the matched parts of image1.
void PFrame::draw_over() {

    for (int i = 0; i < width / block_size; i++) {
        for (int j = 0; j < height / block_size; j++) {
            if (matched_blocks[i][j].valid) {

                for (int x = 0; x < block_size; x++) {
                    for (int y = 0; y < block_size; y++) {
                        image2->set_color(x + matched_blocks[i][j].x_start,
                                          y + matched_blocks[i][j].y_start,
                                          image1->get_color(x + matched_blocks[i][j].x_end,
                                                            y + matched_blocks[i][j].y_end));
                    }
                }

            }
        }
    }


}

void PFrame::force_copy() {
    dandere2x::write_empty(difference_file);
    dandere2x::write_empty(p_frame_file);
}


// Call match_block on every possible block in an image
void PFrame::match_all_blocks() {

    int x = 0;
    int y = 0;

#pragma omp parallel for shared(image1, image2, image2_compressed, matched_blocks) private(x, y)

    for (x = 0; x < width / block_size; x++) {
        for (y = 0; y < height / block_size; y++) {
            match_block(x, y);
        }
    }

}


/**
 * Given an (x,y) pair, find the position of a block of one image within the previous image.
 *
 * If the match is a good find, add it to the list of matched blocks.
 *
 * @param x The x-coordinate of an image
 * @param y The y-coordinate of an image
 */
void PFrame::match_block(int x, int y) {

    // Using the compressed image, determine a good measure of the minimum MSE required for the matched to have.
    double min_mse = ImageUtils::mse(*image2, *image2_compressed,
                                     x * block_size, y * block_size,
                                     x * block_size, y * block_size,
                                     block_size);

    // Compute the MSE of the block at the same (x,y) location.
    double stationary_mse = ImageUtils::mse(*image1, *image2,
                                            x * block_size, y * block_size,
                                            x * block_size, y * block_size,
                                            block_size);

    // If the MSE found at the stationary location is good enough, add it to the list of matched blocks.
    if (stationary_mse <= min_mse) {
        matched_blocks[x][y] = Block(x * block_size, y * block_size, x * block_size, y * block_size, stationary_mse);
        this->count++;
    } else {
        // If the MSE found at the stationary location isn't good enough, conduct a diamond search looking
        // for the blocks match nearby.
        Block result = DiamondSearch::diamond_search_iterative_super(*image2, *image1,
                                                                     x * block_size, y * block_size,
                                                                     x * block_size, y * block_size,
                                                                     min_mse, block_size, step_size, max_checks);

        //If the Diamond Searched block is a good enough match, add it to the list of matched blocks.
        if (result.sum <= min_mse) {
            matched_blocks[x][y] = result;
            this->count++;
        }
    }
}


//write all the matched blocks into a text file.
// Save it as '.temp' initially so D2xPython doesn't read it before
// it's done writing.
void PFrame::write(std::string output_file) {

    std::ofstream out(output_file + ".temp");

    for (int x = 0; x < width / block_size; x++) {
        for (int y = 0; y < height / block_size; y++) {
            if (matched_blocks[x][y].valid) {
                out <<
                    matched_blocks[x][y].x_start << "\n" <<
                    matched_blocks[x][y].y_start << "\n" <<
                    matched_blocks[x][y].x_end << "\n" <<
                    matched_blocks[x][y].y_end << std::endl;
            }
        }
    }

    out.close();

    rename((output_file + ".temp").c_str(), output_file.c_str());


}
