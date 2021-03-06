#include <string>
#include <vector>
#include <math.h>
#include <algorithm>
#include <opencv2/opencv.hpp>

#include "contrast_enhancement.h"
#include "guidedfilter.h"
#include <chrono>

using namespace std;
using namespace cv;

void expo_fuse(vector<Mat> pme, Mat &res) {
	/*
	Implementation of "Fast exposure Fusion using exposedness funtion".

	Parameters: 
		pme: Vector of images to be merged
		res: Output image

	Reference:
		M. Nejati, M. Karimi, S. M. R. Soroushmehr, N. Karimi, S. Samavi and K. Najarian, 
		"Fast exposure fusion using exposedness function," 
		2017 IEEE International Conference on Image Processing (ICIP), Beijing, 2017, pp. 2234-2238.
	*/

	float r = 12, eps = 0.25, sig_l = 0.5, sig_g = 0.2, sig_d = 0.12, alpha = 1.1;
	vector<Mat> W_B, W_D, B, D;

	for (int i = 0; i < pme.size(); i++) {
		Mat img = pme[i].clone();
		img.convertTo(img, CV_32FC3);
		img = img / 255;
		Mat lum;
		cvtColor(img, lum, COLOR_RGB2GRAY, 1);
		double max_v;
		minMaxIdx(lum, nullptr, &max_v, nullptr, nullptr);

		Mat wl;
		float wg;
		Mat base = guidedFilter(lum, lum, r, eps);
		wl = (base - 0.5).mul(base - 0.5) / (-2 * sig_l*sig_l);
		float m = mean(lum)[0];
		wg = ((m - 0.5)*(m - 0.5)) / (-2 * sig_l*sig_l);
		exp(wl, wl);
		wg = exp(wg);
		W_B.push_back(wl * wg);
		B.push_back(base);

		wl.release();
		cvtColor(base, base, COLOR_GRAY2RGB);

		Mat detail = img - base;
		base.release();
		Mat kernel = Mat::ones(7, 7, CV_32FC1)*(1 / 49);
		Mat conved, wd;
		filter2D(lum, conved, -1, kernel);
		wd = (conved - 0.5).mul(conved - 0.5) / (-2 * sig_d*sig_d);
		exp(wd, wd);
		W_D.push_back(wd);
		D.push_back(detail);
	}

	Mat wb_s = W_B[0].clone(), wd_s = W_D[0].clone();
	for (int i = 1; i < pme.size(); i++) {
		add(wb_s, W_B[i], wb_s);
		add(wd_s, W_D[i], wd_s);
	}

	Mat dst = Mat::zeros(pme[0].size(), CV_32FC3);
	for (int i = 0; i < pme.size(); i++) {
		Mat wb, wd;
		divide(W_B[i], wb_s, wb);
		divide(W_D[i], wd_s, wd);

		cvtColor(wd, wd, COLOR_GRAY2RGB);
		multiply(wd, D[i], wd);
		wd *= alpha;

		multiply(wb, B[i], wb);
		cvtColor(wb, wb, COLOR_GRAY2RGB);

		dst += wd + wb;

		W_B[i].release(); W_D[i].release(); D[i].release(); B[i].release();
	}
	res = dst.clone();
}


void gamma(Mat *img, float g) {
	/*
	Gamma Correction

	Parameters:
		img: Image to be gamma corrected
		g: gamma value
	*/

	Mat lookUpTable(1, 256, CV_8U);
	uchar* p = lookUpTable.ptr();
	for (int i = 0; i < 256; ++i)
		p[i] = saturate_cast<uchar>(pow(i / 255.0, g) * 255.0);
	LUT(*img, lookUpTable, *img);
}


void extract(Mat* lum, int regions, Mat* labels) {
	/*
	Segment image into regions based upon identical luminosity values

	Parameters:
		lum: Luminosity component of image to be segmented
		regions: Number of regions
		labels: Number of labels
	*/

	vector<uchar> lums = lum->reshape(0, 1);
	sort(lums.begin(), lums.end());
	int diff = lums[lums.size() - 1] - lums[0];
	int tmp_endpoints = (diff / regions);
	vector<float> temp;
	for (int i = 0; i < regions; i++) {
		temp.push_back((tmp_endpoints*i) + lums[0]);
	}
	vector<uchar> table(256);
	for (int i = 0; i < 256; i++) {
		for (int j = 0; j < regions; j++) {
			int k = regions - j - 1;
			if (j == 0) {
				if (i >= temp[k])
					table[i] = j;
			}
			else {
				if (i >= temp[k] && i < temp[k + 1])
					table[i] = j;
			}

		}
	}

	LUT(*lum, table, *labels);
}


void synEFFromJNI(Mat *prev, Mat *res, float g) {
	/*
	Impleentation of "Automatic exposure compensation using an image segmentation 
	method for single-image-based multi-exposure fusion"

	Note:
		Simple segmentation based upon luminsoity values done instead of GMM as
		described in the paperr for speed purposes.

	Parameters:
		prev: Original Image
		res: Output Image
		g: Value for gamma correction. 2.2 for well lit images. 1/2.2 for dark images

	Reference:
		Kinoshita, Y., & Kiya, H. (2018). Automatic exposure compensation using an image
		segmentation method for single-image-based multi-exposure fusion.
		APSIPA Transactions on Signal and Information Processing, 7, E22. doi:10.1017/ATSIP.2018.26
	*/

	Mat &mprev = *(Mat *) prev;
	Mat &mres = *(Mat *) res;
	
	gamma(&mprev, g);

	//Reducing size of image while performing segmentation to improve speed
	Mat small;
	resize(mprev, small, Size(), 0.05, 0.05);

	Mat yuv;
	cvtColor(small, yuv, COLOR_RGB2YUV);

	Mat chan[3];
	split(yuv, chan);

	yuv.release();

	Mat lum;
	lum = chan[0];

	chan[1].release();
	chan[2].release();

	int regions = 7;
	Mat labels(lum.rows, lum.cols, CV_8UC1);


	//Generate Segments
	extract(&lum, regions, &labels);

	vector<double> a;	//Enhancement Factors
	vector<double> sums(regions, 0);
	vector<double> count(regions, 0);

	//To avoid division by 0
	lum += 1;

	for (int i = 0; i < lum.rows; i++) {
		for (int j = 0; j < lum.cols; j++) {
			double idx = (double)lum.at<uchar>(i, j);
			double eps = 0.003;
			double m = max(idx, eps);
			double v = log(m);
			sums[labels.at<uchar>(i, j)] += v;
			count[labels.at<uchar>(i, j)] += 1;
		}
	}

	labels.release();

	//Calculate enhancement factors
	for (int i = 0; i < regions; i++) {
		double v = sums[i] / (count[i] + 0.003);
		a.push_back(0.18 / exp(v));
	}


	cvtColor(mprev, yuv, COLOR_RGB2YUV);
	split(yuv, chan);
	yuv.release();
	lum.release();
	lum = chan[0];
	chan[1].release();
	chan[2].release();

	//apply enhancement factor
	vector<Mat> exp;
	for (int i = 0; i < regions; i++) {
		Mat temp;
		lum.convertTo(temp, CV_32F);
		temp = temp * a[i];
		exp.push_back(temp.clone());
	}
	
	//tone mapping
	for (int i = 0; i < regions; i++) {
		double max_v;
		int temp;
		minMaxIdx(exp[i], nullptr, &max_v, nullptr, &temp);
		Mat t1 = exp[i] / max_v;
		t1 = t1 + 1;
		Mat t2 = exp[i] + 1;
		t2 = exp[i] / t2;
		Mat t3;
		t3 = t2.mul(t1);
		Mat t4;
		lum.convertTo(t4, CV_32F);
		Mat t5 = t3 / (t4 + 0.003);
		exp[i] = t5.clone();
	}
		
	Mat p;
	mprev.convertTo(p, CV_32F);
	vector<Mat> p_chan;
	split(p, p_chan);
	vector<Mat> pme;
	vector<Mat> temp(3);
	for (int i = 0; i < regions; i++) {
		for (int j = 0; j < 3; j++) {
			temp[j] = p_chan[j].mul(exp[i]);
		}
		exp[i].release();
		Mat temp2;
		merge(temp, temp2);
		Mat temp3;
		temp3 = temp2 * 255;
		temp3.convertTo(temp3, CV_8UC3);
		gamma(&temp3, 1.0/g);
		pme.push_back(temp3.clone());
	}
	pme.push_back(mprev.clone());

	p.release();
	for (int i = 0; i < 3; i++) {
		p_chan[i].release();
	}
	gamma(&mprev, 1.0/g);
		
	//OpenCV Exposure Fusion
	Ptr<MergeMertens> merge = createMergeMertens();
	merge->process(pme, mres);

	//Nejati Exposure Fusion
	//expo_fuse(pme, mres);
	
	mres.convertTo(mres, CV_8UC3, 255, 0);

}

bool is_dark(Mat img) {
	/*
	Checks whether image is dark or not. If mean luminance of image is less than 
	or equal to 85, it is considered to be dark. Value of 85 is empirically selected.

	Parameters:
		img: Input image
	*/

	Mat temp;
	cvtColor(img, temp, COLOR_RGB2HSV);
	Scalar m = mean(temp);
	return m[2] <= 85;
}


int main() {
	Mat prev;
	prev = imread("imgs/34.jpg", 1);

	//reduce size to improve speed and visualize better
	double sm = 0.5;
	resize(prev, prev, Size(), sm, sm);

	Mat res, contr, temp, ptemp = prev.clone(), ctemp;
	float g = 2.2;	//gamma correction value

	contrastEnhancement(ptemp, contr);
	ptemp = prev.clone();
	ctemp = contr.clone();

	if (is_dark(prev)) { 
		//if image is dark, syn ef on original image with g = 1/2,2
		synEFFromJNI(&ptemp, &temp, 1 / g);
	}
	else {
		//If image is bright enogh, use contrast enhanced image with g = 2.2
		synEFFromJNI(&ctemp, &temp, g);
	}
	
	imshow("original", prev);
	imshow("contrast enhanced", contr);
	imshow("syn ef", temp);

	//merge original, contrast enhanced and syn ef images to supress unwanted correction
	vector<Mat> pme = {prev, contr, temp};
	Ptr<MergeMertens> merge = createMergeMertens();
	merge->process(pme, res);

	//expo_fuse(pme, res);

	res.convertTo(res, CV_8UC3, 255, 0);
	imshow("result", res);
	waitKey();
	destroyAllWindows();

	imwrite("test-input/19.jpg", prev);
	imwrite("test-output/19.jpg", res);
	
	return 0;
}