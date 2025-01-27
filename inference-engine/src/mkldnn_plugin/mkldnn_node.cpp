// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "mkldnn_node.h"
#include "mkldnn_extension_mngr.h"
#include "mkldnn_itt.h"

#include "caseless.hpp"
#include <vector>
#include <string>
#include <limits>
#include <cstdint>
#include <unordered_map>

#include <nodes/mkldnn_batchnorm_node.h>
#include <nodes/mkldnn_concat_node.h>
#include <nodes/mkldnn_conv_node.h>
#include <nodes/mkldnn_deconv_node.h>
#include <nodes/mkldnn_eltwise_node.h>
#include <nodes/mkldnn_gemm_node.h>
#include <nodes/mkldnn_fullyconnected_node.h>
#include <nodes/mkldnn_generic_node.h>
#include <nodes/mkldnn_input_node.h>
#include <nodes/mkldnn_lrn_node.h>
#include <nodes/mkldnn_pooling_node.h>
#include <nodes/mkldnn_reorder_node.h>
#include <nodes/mkldnn_reshape_node.h>
#include <nodes/mkldnn_softmax_node.h>
#include <nodes/mkldnn_tile_node.h>
#include <nodes/mkldnn_split_node.h>
#include <nodes/mkldnn_pad_node.h>
#include <nodes/mkldnn_permute_node.h>
#include <nodes/mkldnn_memory_node.hpp>
#include <nodes/mkldnn_mvn_node.h>
#include <nodes/mkldnn_normalize_node.h>
#include <nodes/mkldnn_reduce_node.h>
#include <nodes/mkldnn_tensoriterator_node.h>
#include <nodes/mkldnn_scatter_update_node.h>
#include <nodes/mkldnn_interpolate_node.h>
#include <nodes/mkldnn_strided_slice_node.h>
#include <mkldnn_types.h>
#include <dnnl_types.h>
#include "mkldnn_extension_utils.h"

#include "nodes/common/cpu_memcpy.h"
#include "mkldnn_debug.h"
#include "utils/rt_info/memory_formats_attribute.hpp"

using namespace mkldnn;
using namespace MKLDNNPlugin;
using namespace openvino;

using namespace InferenceEngine::details;
namespace MKLDNNPlugin {
static const InferenceEngine::details::caseless_unordered_map<std::string, Type> type_to_name_tbl = {
        { "Unknown", Unknown },
        { "Input", Input },
        { "Const", Input },
        { "Output", Output },
        { "Reorder", Reorder },
        { "Convolution", Convolution },
        { "ReLU", Eltwise },
        { "GELU", Eltwise },
        { "ELU", Eltwise },
        { "Sigmoid", Eltwise },
        { "Logistic", Eltwise },
        { "TanH", Eltwise },
        { "ReLU6", Eltwise },
        { "Exp", Eltwise },
        { "Not", Eltwise },
        { "Activation", Eltwise },
        { "Clamp", Eltwise },
        { "Swish", Eltwise },
        { "HSwish", Eltwise },
        { "Mish", Eltwise },
        { "HSigmoid", Eltwise },
        { "Round", Eltwise },
        { "ScaleShift", Eltwise },
        { "PReLU", Eltwise },
        { "Norm", Lrn },
        { "LRN", Lrn },
        { "Pooling", Pooling },
        { "FullyConnected", FullyConnected },
        { "InnerProduct", FullyConnected },
        { "Gemm", Gemm },
        { "Softmax", SoftMax },
        { "SoftMax", SoftMax },
        { "Split", Split },
        { "Slice", Split },
        { "Concat", Concatenation },
        { "Deconvolution", Deconvolution },
        { "Eltwise", Eltwise },
        { "Mod", Eltwise },
        { "Power", Eltwise },
        { "Reshape", Reshape },
        { "Tile", Tile },
        { "SimplerNMS", SimplerNMS },
        { "ROIAlign", ROIAlign },
        { "ROIPooling", ROIPooling },
        { "BatchNormalization", BatchNormalization },
        { "Flatten", Flatten },
        { "Pad", Pad },
        { "Permute", Permute },
        { "StridedSlice", StridedSlice },
        { "Copy", Copy },
        { "LSTMCell", RNNCell },
        { "GRUCell", RNNCell },
        { "RNNCell", RNNCell },
        { "LSTMSequence", RNNSeq },
        { "GRUSequence", RNNSeq },
        { "RNNSequence", RNNSeq },
        { "Quantize", Quantize },
        { "FakeQuantize", Quantize },
        { "BinaryConvolution", BinaryConvolution },
        { "DeformableConvolution", DeformableConvolution },
        { "TensorIterator", TensorIterator },
        { "Loop", TensorIterator },
        { "MemoryInput", MemoryInput},  // for construction from name ctor, arbitrary name is used
        { "Memory", MemoryOutput },  // for construction from layer ctor
        { "Convert", Convert },
        { "MVN", MVN},
        { "Normalize", Normalize},
        { "ScatterUpdate", ScatterUpdate},
        { "ScatterElementsUpdate", ScatterElementsUpdate},
        { "ScatterNDUpdate", ScatterNDUpdate},
        { "Interpolate", Interpolate},
        { "ReduceAnd", ReduceAnd},
        { "ReduceL1", ReduceL1},
        { "ReduceL2", ReduceL2},
        { "ReduceLogSum", ReduceLogSum},
        { "ReduceLogSumExp", ReduceLogSumExp},
        { "ReduceMax", ReduceMax},
        { "ReduceMean", ReduceMean},
        { "ReduceMin", ReduceMin},
        { "ReduceOr", ReduceOr},
        { "ReduceProd", ReduceProd},
        { "ReduceSum", ReduceSum},
        { "ReduceSumSquare", ReduceSumSquare},
        { "Erf", Eltwise },
};

Type TypeFromName(const std::string type) {
    auto itType = type_to_name_tbl.find(type);
    if (type_to_name_tbl.end() != itType) {
        return itType->second;
    } else {
        return Unknown;
    }
}

}  //  namespace MKLDNNPlugin

MKLDNNNode::NodesFactory & MKLDNNNode::factory() {
    static NodesFactory factoryInstance;
    return factoryInstance;
}

MKLDNNNode::MKLDNNNode(const InferenceEngine::CNNLayerPtr& layer, const mkldnn::engine& eng,
        MKLDNNWeightsSharing::Ptr &w_cache)
        : selectedPrimitiveDescriptorIndex(-1), permanent(false), temporary(false), constant(ConstantType::Unknown),
          weightCache(w_cache), cnnLayer(layer), engine(eng), name(layer->name), typeStr(layer->type),
          type(TypeFromName(layer->type)), profiling(layer->name) {
    if (!layer->outData.empty()) {
        for (const auto& outData : layer->outData) {
            outDims.emplace_back(outData->getDims());
        }
    } else {
        if (!(CaselessEq<std::string>()(layer->type, "memory") ||
            CaselessEq<std::string>()(layer->type, "memoryinput") ||
            CaselessEq<std::string>()(layer->type, "output") ||
            CaselessEq<std::string>()(layer->type, "reorder") ||
            CaselessEq<std::string>()(layer->type, "convert"))) {
            IE_THROW() << "Inappropriate layer type: " << layer->type << " name: " << layer->name;
        }
    }

    for (const auto& inData : layer->insData) {
        inDims.emplace_back(inData.lock()->getDims());
    }
    if (layer->params.find("PrimitivesPriority") != layer->params.end()) {
        std::istringstream stream(layer->params["PrimitivesPriority"]);
        std::string str;
        while (getline(stream, str, ',')) {
            if (str.substr(0, 4) != "cpu:")
                continue;
            implPriorities.push_back(parse_impl_name(str));
            if (implPriorities[implPriorities.size() - 1] == impl_desc_type::unknown &&
                    str != "cpu:unknown")
                IE_THROW() << "Unsupported CPU implementation " << str << " for node " << getName();
        }
    }

    auto ngraphNode = layer->getNode();
    if (ngraphNode != nullptr) {
        std::string inputMemoryFormats = ngraph::getMLKDNNInputMemoryFormats(ngraphNode);
        if (!inputMemoryFormats.empty()) {
            std::istringstream stream(inputMemoryFormats);
            std::string str;
            while (getline(stream, str, ',')) {
                if (str.substr(0, 4) != "cpu:")
                    continue;
                inputMemoryFormatsFilter.push_back(mkldnn::utils::str2fmt(str.substr(4, str.size()).c_str()));
            }
        }

        std::string outputMemoryFormats = ngraph::getMLKDNNOutputMemoryFormats(ngraphNode);
        if (!outputMemoryFormats.empty()) {
            std::istringstream stream(outputMemoryFormats);
            std::string str;
            while (getline(stream, str, ',')) {
                if (str.substr(0, 4) != "cpu:")
                    continue;
                outputMemoryFormatsFilter.push_back(mkldnn::utils::str2fmt(str.substr(4, str.size()).c_str()));
            }
        }
    }
}

void MKLDNNNode::addEdge(const MKLDNNEdgeWeakPtr& edge) {
    auto edgePtr = edge.lock();
    if (!edgePtr)
        return;
    auto parentPtr = edgePtr->getParent();
    auto childPtr = edgePtr->getChild();
    if (!parentPtr || !childPtr)
        return;

    parentPtr->childEdges.push_back(edge);
    childPtr->parentEdges.push_back(edge);
}

void MKLDNNNode::removeEdge(const MKLDNNEdgeWeakPtr& edge) {
    auto edgePtr = edge.lock();
    if (!edgePtr)
        return;
    auto parentPtr = edgePtr->getParent();
    auto childPtr = edgePtr->getChild();
    if (!parentPtr || !childPtr)
        return;
    for (auto it = childPtr->parentEdges.begin(); it != childPtr->parentEdges.end(); it++) {
        auto parentEdge = (*it).lock();
        if (parentEdge && parentEdge->getChild() == childPtr && parentEdge->getParent() == parentPtr) {
            childPtr->parentEdges.erase(it);
            break;
        }
    }
    for (auto it = parentPtr->childEdges.begin(); it != parentPtr->childEdges.end(); it++) {
        auto childEdge = (*it).lock();
        if (childEdge && childEdge->getChild() == childPtr && childEdge->getParent() == parentPtr) {
            parentPtr->childEdges.erase(it);
            break;
        }
    }
}

void MKLDNNNode::remove() {
    auto parent_edges = parentEdges;
    for (const auto &parentEdge : parent_edges) {
        removeEdge(parentEdge);
    }
    auto child_edges = childEdges;
    for (const auto &childEdge : child_edges) {
        removeEdge(childEdge);
    }
}

bool MKLDNNNode::isEdgesEmpty(const std::vector<MKLDNNEdgeWeakPtr>& edges) const {
    for (auto &edge : edges) {
        if (edge.lock())
            return false;
    }
    return true;
}

void MKLDNNNode::selectOptimalPrimitiveDescriptor() {
    selectPreferPrimitiveDescriptor(getPrimitivesPriority());
}

void MKLDNNNode::selectPreferPrimitiveDescriptor(const std::vector<impl_desc_type>& priority) {
    for (auto& type : priority) {
        int selectedPrimitive = -1;
        int equalsFormatCount = -1;
        for (size_t i = 0; i < getSupportedPrimitiveDescriptors().size(); i++) {
            impl_desc_type supportedType = getSupportedPrimitiveDescriptors()[i].getImplementationType();
            if (type == supportedType) {
                int equalsLocalFormatCount = 0;
                if (getSupportedPrimitiveDescriptors()[i].getConfig().inConfs.size() > getParentEdges().size())
                    continue;
                for (size_t j = 0; j < getSupportedPrimitiveDescriptors()[i].getConfig().inConfs.size(); j++) {
                    auto parentEdge = getParentEdgeAt(j);
                    auto parentPtr = parentEdge->getParent();
                    auto parent_spd = parentPtr->getSelectedPrimitiveDescriptor();

                    if (parent_spd != nullptr && !parent_spd->getConfig().outConfs.empty()) {
                        int inNum = parentEdge->getInputNum();
                        if (inNum < 0 || inNum >= parent_spd->getConfig().outConfs.size()) {
                            inNum = 0;
                        }
                        if (MKLDNNExtensionUtils::initTensorsAreEqual(
                                getSupportedPrimitiveDescriptors()[i].getConfig().inConfs[j].desc,
                                parent_spd->getConfig().outConfs[inNum].desc)) {
                            equalsLocalFormatCount++;
                        }
                    }
                }
                if (equalsLocalFormatCount > equalsFormatCount) {
                    equalsFormatCount = equalsLocalFormatCount;
                    selectedPrimitive = static_cast<int>(i);
                }
            }
        }
        if (selectedPrimitive >= 0) {
            selectPrimitiveDescriptorByIndex(selectedPrimitive);
            return;
        }
    }

    if (getSupportedPrimitiveDescriptors().empty())
        IE_THROW() << "Supported primitive descriptors list is empty for node: " << getName();
    // fallback. If there are no primitives from priority list just select a first
    selectPrimitiveDescriptorByIndex(0);
}

bool MKLDNNNode::canBeInPlace() const {
    if (getParentEdges().size() != 1 || getParentEdgeAt(0)->getParent()->getChildEdges().size() != 1 ||
            (getParentEdgeAt(0)->getParent()->isConstant() && !getParentEdgeAt(0)->getChild()->isConstant()))
        return false;

    // TODO: we need to extend this logic to properly handle all possible inplace conflicts
    if (getParentEdges().size() == 1 && getParentEdgeAt(0)->getParent()->getType() == Reshape) {
        auto reshapeNode = getParentEdgeAt(0)->getParent();
        if (reshapeNode->getParentEdgeAt(0)->getParent()->getChildEdges().size() != 1)
            return false;
    }

    MKLDNNDims dims = getParentEdgeAt(0)->getDims();
    for (size_t cIdx = 0; cIdx < getChildEdges().size(); cIdx++) {
        if (getChildEdgeAt(cIdx)->getDims() != dims) {
            return false;
        }
    }
    return true;
}

void MKLDNNNode::resolveNotAllocatedEdges() {
    const PrimitiveDescInfo *selected_pd = getSelectedPrimitiveDescriptor();
    if (!selected_pd)
        IE_THROW() << "Cannot find selected primitive descriptor for node: " << getName();
    for (size_t i = 0; i < getParentEdges().size() && i < selected_pd->getConfig().inConfs.size(); i++) {
        auto parentEdge = getParentEdgeAt(i);

        if (parentEdge->getStatus() != MKLDNNEdge::Status::NotAllocated || selected_pd->getConfig().inConfs[i].inPlace < 0)
            continue;

        auto * memPtr = reinterpret_cast<char*>(parentEdge->getMemory().GetData());
        parentEdge->getMemoryPtr().reset(new MKLDNNMemory(getEngine()));
        parentEdge->getMemoryPtr()->Create(MKLDNNMemoryDesc(selected_pd->getConfig().inConfs[i].desc), memPtr);

        parentEdge->changeStatus(MKLDNNEdge::Status::Allocated);
    }
    for (size_t i = 0; i < getChildEdges().size() && i < selected_pd->getConfig().outConfs.size(); i++) {
        auto childEdge = getChildEdgeAt(i);

        if (childEdge->getStatus() != MKLDNNEdge::Status::NotAllocated || selected_pd->getConfig().outConfs[i].inPlace < 0)
            continue;

        auto * memPtr = reinterpret_cast<char*>(childEdge->getMemory().GetData());
        childEdge->getMemoryPtr().reset(new MKLDNNMemory(getEngine()));
        childEdge->getMemoryPtr()->Create(MKLDNNMemoryDesc(selected_pd->getConfig().outConfs[i].desc), memPtr);

        childEdge->changeStatus(MKLDNNEdge::Status::Allocated);
    }
}

std::string MKLDNNNode::getPrimitiveDescriptorType() {
    auto selectedPrimitiveDesc = getSelectedPrimitiveDescriptor();

    impl_desc_type type = impl_desc_type::undef;
    if (selectedPrimitiveDesc) {
        type = selectedPrimitiveDesc->getImplementationType();
    }

    std::string str_type;

    auto add_type = [&](std::string t) {
        if (!str_type.empty() && t.c_str()[0] != '_')
            str_type += "_";
        str_type += t;
    };

#define SEARCH_TYPE(_type)                                          \
    if ((type & impl_desc_type::_type) == impl_desc_type::_type)    \
        add_type(#_type)

    SEARCH_TYPE(undef);
    SEARCH_TYPE(reorder);
    SEARCH_TYPE(jit);
    SEARCH_TYPE(gemm);
    SEARCH_TYPE(ref);

    SEARCH_TYPE(avx512);
    SEARCH_TYPE(avx2);
    SEARCH_TYPE(avx);
    SEARCH_TYPE(sse42);
    SEARCH_TYPE(blas);
    SEARCH_TYPE(any);
    SEARCH_TYPE(uni);

    SEARCH_TYPE(winograd);
    SEARCH_TYPE(_dw);
    SEARCH_TYPE(_1x1);

    if (type == impl_desc_type::unknown)
        str_type = "unknown";
    else if (str_type.empty())
        str_type = "undef";

    // adding layer precision to the performance counters as one of the token
    // currently we treat a layer executing in int8 mode if its input is I8 or U8. if input is U8, we still
    // add I8 since I8 is special placeholder. The real calc precision might be quite complex and in most cases
    // it is mixed precision.
    if (selectedPrimitiveDesc) {
        if (!selectedPrimitiveDesc->getConfig().inConfs.empty()) {
            if (selectedPrimitiveDesc->getConfig().inConfs[0].desc.getPrecision() != InferenceEngine::Precision::U8) {
                str_type += "_" + std::string(selectedPrimitiveDesc->getConfig().inConfs[0].desc.getPrecision().name());
            } else {
                str_type += "_I8";
            }
        } else {
            if (selectedPrimitiveDesc->getConfig().outConfs[0].desc.getPrecision() != InferenceEngine::Precision::U8) {
                str_type += "_" + std::string(selectedPrimitiveDesc->getConfig().outConfs[0].desc.getPrecision().name());
            } else {
                str_type += "_I8";
            }
        }
    }

    return str_type;
}

const MKLDNNEdgePtr MKLDNNNode::getParentEdgeAt(size_t idx) const {
    if (idx >= parentEdges.size())
        IE_THROW() << "Node " << getName() << " contains less parent edges than " << idx;
    auto parentEdgePtr = parentEdges[idx].lock();
    if (!parentEdgePtr)
        IE_THROW() << "Node " << getName() << " contains empty parent edge for index " << idx;
    return parentEdgePtr;
}

const MKLDNNEdgePtr MKLDNNNode::getChildEdgeAt(size_t idx) const {
    if (idx >= childEdges.size())
        IE_THROW() << "Node " << getName() << " contains less child edges than " << idx;
    auto childEdgePtr = childEdges[idx].lock();
    if (!childEdgePtr)
        IE_THROW() << "Node " << getName() << " contains empty child edge for index " << idx;
    return childEdgePtr;
}

const std::vector<MKLDNNEdgePtr> MKLDNNNode::getParentEdgesAtPort(size_t idx) const {
    if (idx >= inDims.size())
        IE_THROW() << "Node " << getName() << " contains less input ports than " << idx;

    std::vector<MKLDNNEdgePtr> res;
    for (auto &edge_w : parentEdges) {
        auto edge = edge_w.lock();
        if (!edge)
            IE_THROW() << "Node " << getName() << " contains dead weak ptr";
        if (edge->getOutputNum() == idx) res.push_back(edge);
    }
    return res;
}

const std::vector<MKLDNNEdgePtr> MKLDNNNode::getChildEdgesAtPort(size_t idx) const {
    if (idx >= outDims.size())
        IE_THROW() << "Node " << getName() << " contains less output ports than " << idx;

    std::vector<MKLDNNEdgePtr> res;
    for (auto &edge_w : childEdges) {
        auto edge = edge_w.lock();
        if (!edge)
            IE_THROW() << "Node " << getName() << " contains dead weak ptr";
        if (edge->getInputNum() == idx) res.push_back(edge);
    }
    return res;
}


std::vector<memory::format_tag> MKLDNNNode::getAvailableFormatsForDims(const MKLDNNDims &dims) const {
    if (dims.ndims() == 0)
        return {memory::format_tag::x};
    else if (dims.ndims() == 1)
        return {memory::format_tag::x};
    else if (dims.ndims() == 2)
        return {memory::format_tag::nc};
    else if (dims.ndims() == 3)
        return {memory::format_tag::tnc, memory::format_tag::ntc};
    else if (dims.ndims() == 4)
        return {memory::format_tag::nchw, memory::format_tag::nChw8c, memory::format_tag::nChw16c};
    else if (dims.ndims() == 5)
        return {memory::format_tag::ncdhw, memory::format_tag::nCdhw8c, memory::format_tag::nCdhw16c};
    return {memory::format_tag::any};
}

void MKLDNNNode::execute(mkldnn::stream strm) {
    if (prim) {
        (*prim).execute(strm, primArgs);
    }
}

void MKLDNNNode::initSupportedPrimitiveDescriptors() {
    if (!supportedPrimitiveDescriptors.empty())
        return;

    for (auto& desc : descs) {
        auto itpd = desc.createPrimitiveDescriptorIterator(engine);

        while (static_cast<bool>(itpd)) {
            InferenceEngine::LayerConfig config;
            config.dynBatchSupport = true;
            for (size_t i = 0; i < descInputNumbers(desc); i++) {
                InferenceEngine::DataConfig dataConfig;
                dataConfig.inPlace = -1;
                dataConfig.constant = false;
                dataConfig.desc = MKLDNNExtensionUtils::getUninitTensorDesc(getSrcMemDesc(itpd, i));
                config.inConfs.push_back(dataConfig);
            }

            for (size_t i = 0; i < descOutputNumbers(desc); i++) {
                InferenceEngine::DataConfig dataConfig;
                dataConfig.inPlace = canBeInPlace() ? 0 : -1;
                dataConfig.constant = false;
                dataConfig.desc = MKLDNNExtensionUtils::getUninitTensorDesc(getDstMemDesc(itpd, i));
                config.outConfs.push_back(dataConfig);
            }
            impl_desc_type impl_type = parse_impl_name(itpd.impl_info_str());

            supportedPrimitiveDescriptors.emplace_back(config, impl_type);
            if (!itpd.next_impl())
                break;
        }
    }
}

void MKLDNNNode::filterSupportedPrimitiveDescriptors() {
    // Compare by partial layout descriptor (without particular strides values)
    auto areCompatible = [](const TensorDesc& tdesc, mkldnn::memory::format_tag fmt) {
        TensorDesc fmt_tdesc = MKLDNNMemoryDesc{
            MKLDNNDims(tdesc.getDims()),
            MKLDNNExtensionUtils::IEPrecisionToDataType(tdesc.getPrecision()),
            fmt};

        auto tmp_partial_tdesc = PartialBlkDesc::extractFrom(fmt_tdesc);
        auto actual_partial_tdesc = PartialBlkDesc::extractFrom(tdesc);
        return tmp_partial_tdesc == actual_partial_tdesc;
    };

    if (!inputMemoryFormatsFilter.empty() || !outputMemoryFormatsFilter.empty()) {
        auto itpd = supportedPrimitiveDescriptors.begin();
        while (itpd != supportedPrimitiveDescriptors.end()) {
            const auto &config = itpd->getConfig();
            if (inputMemoryFormatsFilter.size() > config.inConfs.size() || outputMemoryFormatsFilter.size() > config.outConfs.size())
                IE_THROW() << "Incorrect number of input or output memory formats";

            bool isSuitableDesc = true;
            for (int i = 0; i < inputMemoryFormatsFilter.size(); i++) {
                const bool matched = areCompatible(config.inConfs[i].desc, inputMemoryFormatsFilter[i]);
                isSuitableDesc &= matched;
            }
            for (int i = 0; i < outputMemoryFormatsFilter.size(); i++) {
                const bool matched = areCompatible(config.outConfs[i].desc, outputMemoryFormatsFilter[i]);
                isSuitableDesc &= matched;
            }
            if (!isSuitableDesc) {
                itpd = supportedPrimitiveDescriptors.erase(itpd);
            } else {
                itpd++;
            }
        }
    }
}

void MKLDNNNode::initDescriptor(const InferenceEngine::LayerConfig &config) {
    auto* selectedPD = getSelectedPrimitiveDescriptor();
    if (!selectedPD) {
        return;
    }
    std::vector<InferenceEngine::TensorDesc> inDescs;
    for (const auto& inConf : config.inConfs)
        inDescs.push_back(inConf.desc);
    std::vector<InferenceEngine::TensorDesc> outDescs;
    for (const auto& outConf : config.outConfs)
        outDescs.push_back(outConf.desc);
    createDescriptor({inDescs}, {outDescs});

    std::shared_ptr<mkldnn::primitive_attr> attr = initPrimitiveAttr();

    InferenceEngine::LayerConfig rightConfig = selectedPD->getConfig();
    size_t selected_count = 0;
    for (size_t j = 0; j < descs.size(); j++) {
        const auto &desc = descs[j];
        primitive_desc_iterator itpd;
        if (attr == nullptr) {
            itpd = desc.createPrimitiveDescriptorIterator(engine);
        } else {
            itpd = desc.createPrimitiveDescriptorIterator(engine, *(attr.get()));
        }
        while (static_cast<bool>(itpd)) {
            InferenceEngine::LayerConfig cfg;
            cfg.dynBatchSupport = true;
            for (size_t i = 0; i < descInputNumbers(desc); i++) {
                InferenceEngine::DataConfig dataConfig;
                dataConfig.inPlace = canBeInPlace() ? 0 : -1;
                dataConfig.constant = false;
                dataConfig.desc = getSrcMemDesc(itpd, i);
                cfg.inConfs.push_back(dataConfig);
            }

            for (size_t i = 0; i < descOutputNumbers(desc); i++) {
                InferenceEngine::DataConfig dataConfig;
                dataConfig.inPlace = -1;
                dataConfig.constant = false;
                dataConfig.desc = getDstMemDesc(itpd, i);
                cfg.outConfs.push_back(dataConfig);
            }
            impl_desc_type impl_type = parse_impl_name(itpd.impl_info_str());
            if (selected_count == selectedPrimitiveDescriptorIndex) {
                if (impl_type != selectedPD->getImplementationType()) {
                    IE_THROW() << "Cannot get the original layer configuration!";
                }
                rightConfig = cfg;
            }
            if (j == descs.size() - 1) {
                if (impl_type == selectedPD->getImplementationType()) {
                    rightConfig = config;
                }
            }
            selected_count++;
            if (!itpd.next_impl())
                break;
        }
    }

    if (descs.empty()) {
        const auto& selectedConfig = selectedPD->getConfig();
        if (selectedConfig.inConfs.size() != config.inConfs.size() || selectedConfig.outConfs.size() != config.outConfs.size())
            return;

        for (size_t i = 0; i < selectedConfig.inConfs.size(); i++) {
            if (selectedConfig.inConfs[i].desc.getLayout() != InferenceEngine::Layout::ANY &&
                !MKLDNNExtensionUtils::initTensorsAreEqual(selectedConfig.inConfs[i].desc, config.inConfs[i].desc))
                IE_THROW() << "Incorrect descriptor for node: " << getName();
        }

        for (size_t i = 0; i < selectedConfig.outConfs.size(); i++) {
            if (selectedConfig.outConfs[i].desc.getLayout() != InferenceEngine::Layout::ANY &&
                !MKLDNNExtensionUtils::initTensorsAreEqual(selectedConfig.outConfs[i].desc, config.outConfs[i].desc))
                IE_THROW() << "Incorrect descriptor for node: " << getName();
        }
        rightConfig = config;
    }

    selectedPD->getConfig() = rightConfig;
}

InferenceEngine::Blob::Ptr MKLDNNNode::createInternalBlob(InferenceEngine::SizeVector dims, bool weights, bool isGrouped) {
    auto checkSize = [](size_t dst_size, size_t src_size) {
        if (dst_size < src_size) {
            IE_THROW() << "Cannot create internal buffer. Buffer can be overrun.";
        }
    };
    auto * wLayer = dynamic_cast<InferenceEngine::WeightableLayer*>(getCnnLayer().get());
    if (wLayer == nullptr)
        IE_THROW() << "Cannot get weightable layer for node " << getName() << ".";

    InferenceEngine::Blob::Ptr blb = weights ? wLayer->_weights : wLayer->_biases;

    if (blb == nullptr)
        IE_THROW() << "Cannot get internal blob layer for node " << getName() << ".";

    auto intLayout = getWeightsLayoutByDims(dims, isGrouped);

    InferenceEngine::TensorDesc desc(blb->getTensorDesc().getPrecision(), dims, intLayout);

    auto fillInternalBlob = [&](char *data, size_t intBuffSize) {
        size_t offset = blb->byteSize();
        checkSize(intBuffSize, offset);
        cpu_memcpy_s(data, intBuffSize, blb->buffer(), blb->byteSize());
        data += blb->byteSize();
        for (const auto &merged : getMergeWith()) {
            wLayer = dynamic_cast<InferenceEngine::WeightableLayer*>(merged->getCnnLayer().get());
            if (wLayer == nullptr)
                IE_THROW() << "Cannot convert merged weightable layer for node "
                                   << getName() << ".";
            blb = weights ? wLayer->_weights : wLayer->_biases;

            if (blb == nullptr)
                IE_THROW() << "Cannot get internal blob layer for node " << getName() << ".";
            offset += blb->byteSize();
            checkSize(intBuffSize, offset);
            cpu_memcpy_s(data, intBuffSize, blb->buffer(), blb->byteSize());
            data += blb->byteSize();
        }
    };

    Blob::Ptr internalBlob;
    if (blb->getTensorDesc().getPrecision() == Precision::BIN) {
        internalBlob = InferenceEngine::make_shared_blob<int8_t>(desc);
    } else if (blb->getTensorDesc().getPrecision() == Precision::I8) {
        internalBlob = InferenceEngine::make_shared_blob<int8_t>(desc);
    } else if (blb->getTensorDesc().getPrecision() == Precision::I32) {
        internalBlob = InferenceEngine::make_shared_blob<int32_t>(desc);
    } else if (blb->getTensorDesc().getPrecision() == Precision::BF16) {
        internalBlob = InferenceEngine::make_shared_blob<int16_t>(desc);
    } else {
        internalBlob = InferenceEngine::make_shared_blob<float>(desc);
    }
    internalBlob->allocate();
    char *data = internalBlob->buffer();
    size_t intBuffSize = internalBlob->byteSize();

    fillInternalBlob(data, intBuffSize);

    return internalBlob;
}

void MKLDNNNode::prepareMemory(const PrimitiveDescInfo *selected_pd, mkldnn::primitive_desc_iterator& itpd) {
    for (size_t i = 0; i < getChildEdges().size(); i++) {
        auto &dstMemPtr = getChildEdgeAt(i)->getMemoryPtr();
        if (!dstMemPtr || !dstMemPtr->GetPrimitivePtr())
            IE_THROW() << "Destination memory didn't allocate for node " << getName()
                               << " to node " << getChildEdgeAt(i)->getChild()->getName() << ".";
    }
    for (size_t i = 0; i < getParentEdges().size(); i++) {
        auto &srcMemPtr = getParentEdgeAt(i)->getMemoryPtr();
        if (!srcMemPtr || !srcMemPtr->GetPrimitivePtr())
            IE_THROW() << "Destination memory didn't allocate for node " << getName()
                               << " from node " << getParentEdgeAt(i)->getParent()->getName() << ".";
    }
    std::vector<MKLDNNMemoryDesc> intDescs;
    for (auto &it : internalBlobDesc)
        intDescs.push_back(it(itpd, 0));

    internalBlobMemory.clear();
    for (size_t i = 0; i < internalBlobs.size(); i++) {
        const auto &internalBlob = internalBlobs[i];

        auto create = [&] () {
            auto newDesc = MKLDNNMemoryDesc(internalBlob->getTensorDesc());

            MKLDNNMemory memory{ engine };
            memory.Create(newDesc, internalBlob->buffer());

            MKLDNNMemoryPtr _ptr = MKLDNNMemoryPtr(new MKLDNNMemory(engine));
            _ptr->Create(intDescs[i]);
            _ptr->SetData(memory);

            return _ptr;
        };

        MKLDNNMemoryPtr ptr;
        if (weightCache != nullptr) {
            const uint64_t data_hash = weightCache->GetHashFunc().hash(
                    internalBlob->buffer(), internalBlob->byteSize());

            const std::string string_hash = name + "_" + std::to_string(i)
                                            + "_" + std::to_string(internalBlob->byteSize())
                                            + "_" + std::to_string(data_hash);

            ptr = *weightCache->findOrCreate(string_hash, create);
        } else {
            ptr = create();
        }

        internalBlobMemory.push_back(ptr);
    }
}

bool MKLDNNNode::isInplace() const {
    auto selected_pd = getSelectedPrimitiveDescriptor();
    if (selected_pd == nullptr)
        IE_THROW() << "Preferable primitive descriptor is not set.";
    auto config = selected_pd->getConfig();

    for (auto &in : config.inConfs) if (in.inPlace >= 0) return true;
    for (auto &out : config.outConfs) if (out.inPlace >= 0) return true;
    return false;
}

bool MKLDNNNode::isConstant() {
    if (constant == ConstantType::Unknown) {
        std::vector<MKLDNNNodePtr> checkNodes;
        for (size_t i = 0; i < getChildEdges().size(); i++) {
            checkNodes.push_back(getChildEdgeAt(i)->getChild());
        }
        while (constant != ConstantType::NoConst && !checkNodes.empty()) {
            constant = checkNodes.front()->checkConstant(LOOK_DOWN, checkNodes);
            checkNodes.erase(checkNodes.begin());
        }
        if (constant != ConstantType::Const) {
            constant = ConstantType::Unknown;
            checkNodes.clear();
            for (size_t i = 0; i < getParentEdges().size(); i++) {
                checkNodes.push_back(getParentEdgeAt(i)->getParent());
            }
            while (constant != ConstantType::NoConst && !checkNodes.empty()) {
                constant = checkNodes.front()->checkConstant(LOOK_UP, checkNodes);
                checkNodes.erase(checkNodes.begin());
            }
        }
        if (constant == ConstantType::Unknown)
            constant = ConstantType::NoConst;
    }
    return constant == ConstantType::Const;
}

MKLDNNNode::ConstantType MKLDNNNode::checkConstant(LOOK look, std::vector<MKLDNNNodePtr>& checkNodes) {
    if (constant == ConstantType::Unknown) {
        if (look == LOOK_DOWN) {
            for (size_t i = 0; i < getChildEdges().size(); i++) {
                if (std::find(checkNodes.begin(), checkNodes.end(), getChildEdgeAt(i)->getChild()) == checkNodes.end())
                    checkNodes.push_back(getChildEdgeAt(i)->getChild());
            }
        } else {
            for (size_t i = 0; i < getParentEdges().size(); i++) {
                if (std::find(checkNodes.begin(), checkNodes.end(), getParentEdgeAt(i)->getParent()) == checkNodes.end())
                    checkNodes.push_back(getParentEdgeAt(i)->getParent());
            }
        }
    }
    return constant;
}

void MKLDNNNode::addOriginalLayer(const InferenceEngine::CNNLayerPtr &layer) {
    if (!layer) return;
    if (originalLayers.empty()) {
        originalLayers = layer->name;
    } else {
        originalLayers += "," + layer->name;
    }
}

void MKLDNNNode::cleanup() {
    internalBlobs.clear();
    cnnLayer.reset();

    for (auto it : fusedWith) {
        it->cleanup();
    }

    for (auto it : mergedWith) {
        it->cleanup();
    }
}

const std::vector<impl_desc_type>& MKLDNNNode::getPrimitivesPriority() {
    std::vector<impl_desc_type> priorities = {
            impl_desc_type::unknown,
            impl_desc_type::jit_uni_dw,
            impl_desc_type::jit_uni_1x1,
            impl_desc_type::jit_uni,
            impl_desc_type::jit_avx512_dw,
            impl_desc_type::jit_avx512_1x1,
            impl_desc_type::jit_avx512,
            impl_desc_type::jit_avx2_dw,
            impl_desc_type::jit_avx2_1x1,
            impl_desc_type::jit_avx2,
            impl_desc_type::jit_avx_dw,
            impl_desc_type::jit_avx_1x1,
            impl_desc_type::jit_avx,
            impl_desc_type::jit_sse42_dw,
            impl_desc_type::jit_sse42_1x1,
            impl_desc_type::jit_sse42,
            impl_desc_type::gemm_any,
            impl_desc_type::gemm_blas,
            impl_desc_type::gemm_avx512,
            impl_desc_type::gemm_avx2,
            impl_desc_type::gemm_avx,
            impl_desc_type::gemm_sse42,
            impl_desc_type::jit_gemm,
            impl_desc_type::ref_any,
            impl_desc_type::ref,
    };
    for (const auto& impl : priorities) {
        if (std::find(implPriorities.begin(), implPriorities.end(), impl) == implPriorities.end())
            implPriorities.push_back(impl);
    }
    return implPriorities;
}

bool MKLDNNNode::isUninitTensorDesc(const InferenceEngine::TensorDesc& desc) const {
    if (desc.getLayout() == InferenceEngine::Layout::ANY)
        return true;

    if (desc.getBlockingDesc().getOffsetPadding() == std::numeric_limits<size_t>::max())
        return true;

    for (size_t i = 0; i < desc.getBlockingDesc().getOrder().size(); i++) {
        if (desc.getBlockingDesc().getOffsetPaddingToData()[i] == std::numeric_limits<size_t>::max() ||
                desc.getBlockingDesc().getStrides()[i] == std::numeric_limits<size_t>::max())
            return true;
    }

    return false;
}

InferenceEngine::TensorDesc MKLDNNNode::getConfiguredInputDesc(const InferenceEngine::LayerConfig& config, size_t idx) const {
    if (!isUninitTensorDesc(config.inConfs[idx].desc))
        return config.inConfs[idx].desc;

    int num = getParentEdgeAt(idx)->getInputNum();
    auto *selectedPD = getParentEdgeAt(idx)->getParent()->getSelectedPrimitiveDescriptor();
    if (!selectedPD)
        IE_THROW() << "Cannot get selected primitive descriptor for node: " << getParentEdgeAt(idx)->getParent()->getName();

    if (selectedPD->getConfig().outConfs.size() <= num)
        num = 0;

    if (config.inConfs[idx].inPlace >= 0) {
        return getConfiguredOutputDesc(config, static_cast<size_t>(config.inConfs[idx].inPlace));
    }

    if (num >= 0) {
        auto parentConf = selectedPD->getConfig().outConfs[num];
        parentConf.desc.setPrecision(config.inConfs[idx].desc.getPrecision());
        if (isUninitTensorDesc(parentConf.desc) && parentConf.inPlace >= 0)
            getParentEdgeAt(idx)->getParent()->initOptimalPrimitiveDescriptor();
        parentConf = getParentEdgeAt(idx)->getParent()->getSelectedPrimitiveDescriptor()->getConfig().outConfs[num];
        if (!isUninitTensorDesc(parentConf.desc) &&
            MKLDNNExtensionUtils::initTensorsAreEqual(parentConf.desc, config.inConfs[idx].desc)) {
            return parentConf.desc;
        }

        if (config.inConfs[idx].desc.getLayout() == InferenceEngine::Layout::ANY &&
            parentConf.desc.getLayout() != InferenceEngine::Layout::ANY) {
            return InferenceEngine::TensorDesc(parentConf.desc.getPrecision(),
                                               parentConf.desc.getDims(), {
                                                       parentConf.desc.getBlockingDesc().getBlockDims(),
                                                       parentConf.desc.getBlockingDesc().getOrder()
                                               });
        }
    }

    if (config.inConfs[idx].desc.getLayout() != InferenceEngine::Layout::ANY) {
        return InferenceEngine::TensorDesc(config.inConfs[idx].desc.getPrecision(),
                                           config.inConfs[idx].desc.getDims(), {
                                                   config.inConfs[idx].desc.getBlockingDesc().getBlockDims(),
                                                   config.inConfs[idx].desc.getBlockingDesc().getOrder()
                                           });
    }

    return InferenceEngine::TensorDesc(config.inConfs[idx].desc.getPrecision(),
                                       config.inConfs[idx].desc.getDims(),
                                       InferenceEngine::TensorDesc::getLayoutByDims(config.inConfs[idx].desc.getDims()));
}

InferenceEngine::TensorDesc MKLDNNNode::getConfiguredOutputDesc(const InferenceEngine::LayerConfig& config, size_t idx) const {
    if (!isUninitTensorDesc(config.outConfs[idx].desc))
        return config.outConfs[idx].desc;

    int num = getChildEdgeAt(idx)->getOutputNum();
    auto *selectedPD = getChildEdgeAt(idx)->getChild()->getSelectedPrimitiveDescriptor();
    if (!selectedPD)
        IE_THROW() << "Cannot get selected primitive descriptor for node: " << getChildEdgeAt(idx)->getChild()->getName();

    if (selectedPD->getConfig().inConfs.size() <= num)
        num = 0;

    if (config.outConfs[idx].inPlace >= 0) {
        return getConfiguredInputDesc(config, static_cast<size_t>(config.outConfs[idx].inPlace));
    }

    if (num >= 0) {
        auto childConf = selectedPD->getConfig().inConfs[num];
        childConf.desc.setPrecision(config.outConfs[idx].desc.getPrecision());
        if (isUninitTensorDesc(childConf.desc) && childConf.inPlace >= 0)
            getChildEdgeAt(idx)->getChild()->initOptimalPrimitiveDescriptor();
        childConf = getChildEdgeAt(idx)->getChild()->getSelectedPrimitiveDescriptor()->getConfig().inConfs[num];
        if (!isUninitTensorDesc(childConf.desc) &&
            MKLDNNExtensionUtils::initTensorsAreEqual(childConf.desc, config.outConfs[idx].desc)) {
            return childConf.desc;
        }
        if (config.outConfs[idx].desc.getLayout() == InferenceEngine::Layout::ANY &&
            childConf.desc.getLayout() != InferenceEngine::Layout::ANY) {
            return InferenceEngine::TensorDesc(childConf.desc.getPrecision(),
                                               childConf.desc.getDims(), {
                                                       childConf.desc.getBlockingDesc().getBlockDims(),
                                                       childConf.desc.getBlockingDesc().getOrder()
                                               });
        }
    }

    if (config.outConfs[idx].desc.getLayout() != InferenceEngine::Layout::ANY) {
        return InferenceEngine::TensorDesc(config.outConfs[idx].desc.getPrecision(),
                                                                config.outConfs[idx].desc.getDims(), {
                                                                        config.outConfs[idx].desc.getBlockingDesc().getBlockDims(),
                                                                        config.outConfs[idx].desc.getBlockingDesc().getOrder()
                                                                });
    }

    return InferenceEngine::TensorDesc(config.outConfs[idx].desc.getPrecision(),
                                       config.outConfs[idx].desc.getDims(),
                                       InferenceEngine::TensorDesc::getLayoutByDims(config.outConfs[idx].desc.getDims()));
}

void MKLDNNNode::initOptimalPrimitiveDescriptor() {
    auto selected_pd = getSelectedPrimitiveDescriptor();
    if (selected_pd == nullptr)
        IE_THROW() << "Preferable primitive descriptor is not set.";
    auto config = selected_pd->getConfig();
    if (!isInitConfig(config)) {
        for (size_t i = 0; i < config.inConfs.size(); i++) {
            // TensorDescriptor constructor which is called inside getConfiguredInputDesc incorrectly computes offset field.
            // What's why MKLDNNMemoryDesc routine is used to reinitialize TD with expected offset values.
            config.inConfs[i].desc = MKLDNNMemoryDesc(getConfiguredInputDesc(config, i));
        }

        for (size_t i = 0; i < config.outConfs.size(); i++) {
            // TensorDescriptor constructor which is called inside getConfiguredOutputDesc incorrectly computes offset field.
            // What's why MKLDNNMemoryDesc routine is used to reinitialize TD with expected offset values.
            config.outConfs[i].desc = MKLDNNMemoryDesc(getConfiguredOutputDesc(config, i));
        }

        initDescriptor(config);
    } else if (getType() != RNNSeq && getType() != RNNCell) {
        initDescriptor(config);
    }
}

bool MKLDNNNode::isInitConfig(const InferenceEngine::LayerConfig& config) const {
    for (const auto& configs : {config.inConfs, config.outConfs}) {
        for (const auto &dc : configs) {
            if (isUninitTensorDesc(dc.desc))
                return false;
        }
    }
    return true;
}

MKLDNNMemoryDesc MKLDNNNode::getSrcMemDesc(mkldnn::primitive_desc_iterator &primitive_desc_it, size_t idx) {
    InferenceEngine::TensorDesc desc = MKLDNNMemoryDesc(primitive_desc_it.src_desc(idx));
    if (desc.getLayout() == InferenceEngine::Layout::ANY)
        return MKLDNNMemoryDesc(InferenceEngine::TensorDesc(desc.getPrecision(),
                                                            getParentEdgeAt(idx)->getDims().ToSizeVector(),
                                                            desc.getLayout()));
    else
        return MKLDNNMemoryDesc(InferenceEngine::TensorDesc(desc.getPrecision(),
                                                            getParentEdgeAt(idx)->getDims().ToSizeVector(),
                                                            desc.getBlockingDesc()));
}

MKLDNNMemoryDesc MKLDNNNode::getDstMemDesc(mkldnn::primitive_desc_iterator &primitive_desc_it, size_t idx) {
    InferenceEngine::TensorDesc desc = MKLDNNMemoryDesc(primitive_desc_it.dst_desc(idx));
    if (desc.getLayout() == InferenceEngine::Layout::ANY)
        return MKLDNNMemoryDesc(InferenceEngine::TensorDesc(desc.getPrecision(),
                                                            getChildEdgeAt(idx)->getDims().ToSizeVector(),
                                                            desc.getLayout()));
    else
        return MKLDNNMemoryDesc(InferenceEngine::TensorDesc(desc.getPrecision(),
                                                            getChildEdgeAt(idx)->getDims().ToSizeVector(),
                                                            desc.getBlockingDesc()));
}

int MKLDNNNode::batchToProcess() {
    return dynBatchLim == 0 ? getMaxBatch() : std::min<int>(getMaxBatch(), dynBatchLim);
}

int MKLDNNNode::getMaxBatch() {
    // FIXME: batch != 0 dims number
    if (!inDims.empty()) {
        if (inDims[0].ndims())
            return inDims[0][0];
        else
            return 1;
    }
    if (!outDims.empty() && outDims[0].ndims()) {
        if (outDims[0].ndims())
            return outDims[0][0];
        else
            return 1;
    }
    return 0;
}

void MKLDNNNode::setDynamicBatchLim(int lim) {
    dynBatchLim = lim;

    auto setDynamicBatch = [this](int argType, int newBatch) {
        auto param = primArgs.find(argType);
        if (param != primArgs.end()) {
            auto oldMem = param->second;
            mkldnn::memory::desc newMemDesc(oldMem.get_desc());
            newMemDesc.data.dims[0] = newBatch;
            newMemDesc.data.padded_dims[0] = newBatch;
            mkldnn::memory newMem(newMemDesc, oldMem.get_engine(), oldMem.get_data_handle());
            primArgs.at(argType) = newMem;
        }
    };

    if (!primArgs.empty()) {
        int newBatch = batchToProcess();
        setDynamicBatch(DNNL_ARG_SRC, newBatch);
        setDynamicBatch(DNNL_ARG_DST, newBatch);
        setDynamicBatch(DNNL_ARG_DIFF_SRC, newBatch);
        setDynamicBatch(DNNL_ARG_DIFF_DST, newBatch);
    }
}

bool MKLDNNNode::isFusedWith(Type fusedNodeType) const {
    for (auto fusedNode : fusedWith) {
        if (fusedNode->type == fusedNodeType)
            return true;
    }

    return false;
}

Layout MKLDNNNode::getWeightsLayoutByDims(SizeVector dims, bool isGrouped) {
    switch (dims.size()) {
        case 0:
            return Layout::SCALAR;
        case 1:
            return Layout::C;
        case 2:
            return Layout::NC;
        case 3:
            return Layout::CHW;
        case 4:
            return Layout::OIHW;
        case 5:
            return isGrouped ? Layout::GOIHW : Layout::OIDHW;
        case 6:
            return isGrouped ? Layout::GOIDHW : Layout::BLOCKED;
        default:
            return Layout::BLOCKED;
    }
}

void MKLDNNNode::appendPostOps(mkldnn::post_ops& ops) {
    IE_THROW() << "Fusing of " << this->getType() << " operation is not implemented";
}

std::vector<InferenceEngine::Precision> MKLDNNNode::getInputPrecisions() const {
    std::vector<InferenceEngine::Precision> inputPrecisions;
    for (size_t i = 0; i < getParentEdges().size(); i++) {
        auto parentEdge = getParentEdgeAt(i);
        if (parentEdge && parentEdge->getStatus() == MKLDNNEdge::Status::Validated) {
            inputPrecisions.emplace_back(MKLDNNExtensionUtils::DataTypeToIEPrecision((parentEdge->getMemoryPtr()->GetDataType())));
        }
    }
    return inputPrecisions;
}

std::vector<InferenceEngine::Precision> MKLDNNNode::getOutputPrecisions() const {
    std::vector<InferenceEngine::Precision> outputPrecisions;
    for (size_t i = 0; i < getChildEdges().size(); i++) {
        auto childEdge = getChildEdgeAt(i);
        if (childEdge && childEdge->getStatus() == MKLDNNEdge::Status::Validated) {
            outputPrecisions.emplace_back(MKLDNNExtensionUtils::DataTypeToIEPrecision((childEdge->getMemoryPtr()->GetDataType())));
        }
    }
    return outputPrecisions;
}

InferenceEngine::Precision MKLDNNNode::getRuntimePrecision() const {
    // Base implementation consider precision only on data path and
    // assumes it is placed on 0-th port (which is true for almost all layers)
    InferenceEngine::Precision runtimePrecision = Precision::UNSPECIFIED;
    auto inputPrecisions = getInputPrecisions();
    if (!inputPrecisions.empty()) {
        runtimePrecision = inputPrecisions[0];
    } else {
        auto outputPrecisions = getOutputPrecisions();
        if (!outputPrecisions.empty()) {
            runtimePrecision = outputPrecisions[0];
        }
    }

    return runtimePrecision;
}

MKLDNNNode* MKLDNNNode::NodesFactory::create(const InferenceEngine::CNNLayerPtr& layer, const mkldnn::engine& eng,
                                             const MKLDNNExtensionManager::Ptr& extMgr, MKLDNNWeightsSharing::Ptr &w_cache) {
    MKLDNNNode *newNode = nullptr;

    std::unique_ptr<MKLDNNNode> ol(createNodeIfRegistered(MKLDNNPlugin, Generic, layer, eng, w_cache));
    if (ol != nullptr && ol->created(extMgr))
        newNode = ol.release();

    if (newNode == nullptr) {
        std::unique_ptr<MKLDNNNode> ol(createNodeIfRegistered(MKLDNNPlugin, TypeFromName(layer->type), layer, eng, w_cache));
        if (ol != nullptr && ol->created(extMgr))
            newNode = ol.release();
    }

    //  WA-start : TI node requires all attributes to construct internal subgpath
    //             including extManager, socket and mkldnn::eng.
    MKLDNNTensorIteratorNode *ti = dynamic_cast<MKLDNNTensorIteratorNode*>(newNode);
    if (ti != nullptr)
        ti->setExtManager(extMgr);
    //  WA-end

    if (!newNode)
        IE_THROW() << "Unsupported primitive of type: " << layer->type << " name: " << layer->name;

    return newNode;
}
