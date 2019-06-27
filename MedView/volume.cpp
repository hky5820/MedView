#include "volume.h"
#include "gdcmScanner.h"
#include "gdcmImageReader.h"
#include "gdcmPixmap.h"
#include "gdcmAttribute.h"

namespace vdcm {
	Volume* read(const char* dirname) {

		// Directory 
		gdcm::Directory dir;
		dir.Load(dirname, false);

		// File List of Directory
		gdcm::Directory::FilenamesType files = dir.GetFilenames();

		if (files.size() == 0) {
			throw std::runtime_error("ERROR! Empty dir\n");
		}

		// For debug
		printf("files %d\n", files.size());
		
		// Read multi-frame DICOMs
		Volume *volume = new Volume();

		volume->m_depth = files.size();

		for (auto filename : files) {
			
			// Read DICOM file
			gdcm::ImageReader reader;
			reader.SetFileName(filename.c_str());
			if (!reader.Read()) {
				throw std::runtime_error("ERROR! None file\n");
			}

			// Save some attributes of DICOM slice
			if (volume->m_width == 0 || volume->m_height == 0) {
				const gdcm::File &f = reader.GetFile();
				const gdcm::DataSet &ds = f.GetDataSet();

				gdcm::Attribute<0x0028, 0x0010> at_rows;
				gdcm::Attribute<0x0028, 0x0011> at_cols;
				at_rows.Set(ds);
				at_cols.Set(ds);
				volume->m_height = at_rows.GetValue();
				volume->m_width = at_cols.GetValue();
			}

			const gdcm::Pixmap &image = reader.GetPixmap();
			int length = image.GetBufferLength();
			
			uint16_t* buffer = new uint16_t[length/2];
			char* temp_buffer = new char[length];
			image.GetBuffer(temp_buffer);

			for (int i = 0; i < length; i+=2) {
				uint16_t val = temp_buffer[i] | (uint16_t)temp_buffer[i+1] << 8;
				buffer[i/2] = val;
			}
			
			std::vector<uint16_t> slice(buffer, buffer + length/2);

			volume->m_volume_data.push_back(slice);
		}

		return volume;
	}

	Volume::Volume() : m_width(0),m_height(0),m_depth(0) {

	}

	std::vector<uint16_t> Volume::getAxialSlice(int idx) {

		return m_volume_data[idx];
	}

	std::vector<uint16_t> Volume::getCoronalSlice(int idx) {
		std::vector<uint16_t> slice;

		for (int i = 0; i < m_depth; i++) {
			for (int j = 0; j < m_width; j++) {
				uint16_t val = m_volume_data[i][idx * m_width + j];
				slice.push_back(val);
			}
		}

		return slice;
	}

	std::vector<uint16_t> Volume::getSagittalSlice(int idx) {
		std::vector<uint16_t> slice;

		for (int i = 0; i < m_depth; i++) {
			for (int j = 0; j < m_height; j++) {
				uint16_t val = m_volume_data[i][j * m_width + idx];
				slice.push_back(val);
			}

		}

		return slice;
	}

	int Volume::getWidth() {
		return m_width;
	}
	int Volume::getHeight() {
		return m_height;
	}
	int Volume::getDepth() {
		return m_depth;
	}

}